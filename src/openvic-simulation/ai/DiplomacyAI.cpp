#include "DiplomacyAI.hpp"

#include <algorithm>
#include <array>
#include <unordered_map>
#include <vector>

#include "openvic-simulation/DefinitionManager.hpp"
#include "openvic-simulation/InstanceManager.hpp"
#include "openvic-simulation/country/CountryInstance.hpp"
#include "openvic-simulation/diplomacy/CountryRelation.hpp"
#include "openvic-simulation/economy/GoodInstance.hpp"
#include "openvic-simulation/map/MapDefinition.hpp"
#include "openvic-simulation/map/MapInstance.hpp"
#include "openvic-simulation/map/ProvinceInstance.hpp"
#include "openvic-simulation/map/State.hpp"
#include "openvic-simulation/military/UnitInstanceGroup.hpp"
#include "openvic-simulation/military/War.hpp"
#include "openvic-simulation/utility/Logger.hpp"

using namespace OpenVic;

namespace {
	using ai_goal_t = CountryInstance::ai_goal_t;

	OpenVicDiplomacyDefines const& defines_of(InstanceManager const& instance_manager) {
		return instance_manager.definition_manager.get_define_manager().get_openvic_defines().diplomacy;
	}

	// Deterministic "dice" in [0, 100) from the country, the date and a salt (as in CountryAI)
	int dice(CountryInstance const& country, const Date date, const std::uint64_t salt) {
		std::uint64_t x = (static_cast<std::uint64_t>(type_safe::get(country.index)) + 1) * 0x9E3779B97F4A7C15ULL;
		x ^= static_cast<std::uint64_t>((date - Date {}).to_int()) * 0xBF58476D1CE4E5B9ULL;
		x ^= salt * 0x94D049BB133111EBULL;
		x ^= x >> 29;
		x *= 0xBF58476D1CE4E5B9ULL;
		x ^= x >> 32;
		return static_cast<int>(x % 100);
	}

	bool is_active(CountryInstance const& country) {
		return country.exists() && !country.is_rebel_country() && !country.get_owned_provinces().empty();
	}

	bool is_great_power(OpenVicDiplomacyDefines const& defines, CountryInstance const& country) {
		const std::size_t rank = country.get_total_rank();
		return rank > 0 && rank <= static_cast<std::size_t>(std::max(0, defines.great_power_rank));
	}

	bool is_small(OpenVicDiplomacyDefines const& defines, CountryInstance const& country) {
		const std::size_t rank = country.get_total_rank();
		return rank == 0 || rank > static_cast<std::size_t>(std::max(0, defines.secondary_power_rank));
	}

	// The land neighbours of every country (by the index of the country), sorted by the index
	std::vector<std::vector<CountryInstance*>> land_neighbours(InstanceManager& instance_manager) {
		CountryInstanceManager& countries = instance_manager.get_country_instance_manager();
		std::vector<std::vector<CountryInstance*>> result(countries.get_country_instances().size());
		MapInstance& map_instance = instance_manager.get_map_instance();
		for (ProvinceInstance& province : map_instance.get_province_instances()) {
			CountryInstance* const owner = province.get_owner();
			if (owner == nullptr) {
				continue;
			}
			for (ProvinceDefinition::adjacency_t const& adjacency : province.province_definition.get_adjacencies()) {
				if (adjacency.get_type() != ProvinceDefinition::adjacency_t::type_t::LAND || adjacency.get_to().is_water()) {
					continue;
				}
				CountryInstance* const other = map_instance.get_province_instance_by_definition(adjacency.get_to()).get_owner();
				if (other == nullptr || other == owner) {
					continue;
				}
				result[type_safe::get(owner->index)].push_back(other);
			}
		}
		for (std::vector<CountryInstance*>& list : result) {
			std::sort(list.begin(), list.end(), [](CountryInstance const* a, CountryInstance const* b) {
				return type_safe::get(a->index) < type_safe::get(b->index);
			});
			list.erase(std::unique(list.begin(), list.end()), list.end());
		}
		return result;
	}

	bool contains(std::vector<CountryInstance*> const& list, CountryInstance const* country) {
		return std::binary_search(list.begin(), list.end(), country, [](CountryInstance const* a, CountryInstance const* b) {
			return type_safe::get(a->index) < type_safe::get(b->index);
		});
	}

	// The interest of a country in another, worked out once a month (monthly_update sets the cache for its run, it is empty outside of it)
	std::unordered_map<std::uint64_t, fixed_point_t>* interest_cache = nullptr;

	fixed_point_t cached_interest(InstanceManager const& instance_manager, CountryInstance const& country, CountryInstance const& other) {
		if (interest_cache == nullptr) {
			return DiplomacyAI::interest(instance_manager, country, other);
		}
		const std::uint64_t key = (static_cast<std::uint64_t>(type_safe::get(country.index)) << 32) | static_cast<std::uint64_t>(type_safe::get(other.index));
		const auto it = interest_cache->find(key);
		if (it != interest_cache->end()) {
			return it->second;
		}
		const fixed_point_t value = DiplomacyAI::interest(instance_manager, country, other);
		interest_cache->emplace(key, value);
		return value;
	}

	// The capitals of the two are at most AI_INTEREST_DISTANCE apart
	bool close(InstanceManager const& instance_manager, OpenVicDiplomacyDefines const& defines, CountryInstance const& a, CountryInstance const& b) {
		if (a.get_capital() == nullptr || b.get_capital() == nullptr) {
			return false;
		}
		MapDefinition const& map_definition = static_cast<MapDefinition const&>(instance_manager.get_map_instance());
		return map_definition.calculate_distance_between(a.get_capital()->province_definition, b.get_capital()->province_definition)
			<= fixed_point_t(std::max(1, defines.ai_interest_distance));
	}

	// The military power, never below a small number (a ratio of powers stays defined)
	fixed_point_t power_of(CountryInstance const& country) {
		// the derived state is read without tracking (it is not changed)
		return std::max(const_cast<CountryInstance&>(country).military_power.get_untracked(), fixed_point_t::_1);
	}

	// How dangerous the other is: its power over ours x (150 - the relation) / 150 (a third to twice), less when it is far
	fixed_point_t threat(
		InstanceManager const& instance_manager, OpenVicDiplomacyDefines const& defines, CountryInstance const& country, CountryInstance const& other,
		const bool neighbour
	) {
		if (&country == &other || country.has_alliance_with(other)) {
			return fixed_point_t::_0;
		}
		if (!neighbour && !is_great_power(defines, other)) {
			return fixed_point_t::_0;
		}
		const std::int32_t relation = country.get_relations_with(other);
		const fixed_point_t relation_factor = std::clamp(fixed_point_t(150 - relation) / 150, fixed_point_t::_1 / 3, fixed_point_t(2));
		fixed_point_t value = power_of(other) / power_of(country) * relation_factor;
		if (!neighbour) {
			// a great power that is not a neighbour threatens as much as the country interests it (Argentina does not fear Russia)
			value = defines.ai_far_threat.of(value) * cached_interest(instance_manager, other, country);
		}
		return value;
	}

	// The biggest threat of the country among its neighbours and the great powers (nullptr: none above AI_THREAT_MIN)
	CountryInstance* biggest_threat(
		InstanceManager& instance_manager, OpenVicDiplomacyDefines const& defines, CountryInstance const& country,
		std::vector<CountryInstance*> const& neighbours, fixed_point_t* value_out = nullptr
	) {
		CountryInstance* best = nullptr;
		fixed_point_t best_value = defines.ai_threat_min.fixed();
		for (CountryInstance& other : instance_manager.get_country_instance_manager().get_country_instances()) {
			if (!is_active(other)) {
				continue;
			}
			// a neighbour by a colony far away counts as far (its threat goes by its interest in us)
			const bool neighbour = contains(neighbours, &other) && close(instance_manager, defines, country, other);
			const fixed_point_t value = threat(instance_manager, defines, country, other, neighbour);
			if (value > best_value) {
				best_value = value;
				best = &other;
			}
		}
		if (value_out != nullptr) {
			*value_out = best != nullptr ? best_value : fixed_point_t::_0;
		}
		return best;
	}

	// The best ally against the threat: liked enough, not allied with the threat, at peace with us; the ones that fear the same country first
	CountryInstance* alliance_candidate(
		InstanceManager& instance_manager, OpenVicDiplomacyDefines const& defines, CountryInstance const& country, CountryInstance const& threat_country,
		std::vector<std::vector<CountryInstance*>> const& neighbours, const std::int32_t min_relation
	) {
		CountryRelationManager const& relations = instance_manager.get_country_relation_manager();
		CountryInstance* best = nullptr;
		fixed_point_t best_score = fixed_point_t::_0;
		for (CountryInstance& other : instance_manager.get_country_instance_manager().get_country_instances()) {
			if (&other == &country || &other == &threat_country || !is_active(other) || country.has_alliance_with(other)
				|| other.has_alliance_with(threat_country) || country.is_at_war_with(other) || other.is_at_war()
				|| country.get_relations_with(other) < min_relation
				|| relations.get_alliance_count(&other) >= static_cast<std::size_t>(std::max(0, defines.ai_max_alliances))) {
				continue;
			}
			const bool shares_threat = contains(neighbours[type_safe::get(other.index)], &threat_country);
			const bool is_neighbour = contains(neighbours[type_safe::get(country.index)], &other);
			// an ally near us: a neighbour, a country near us that fears the same, a great power that is interested in us
			// (a neighbour by a colony far away is not near: the capitals at most AI_INTEREST_DISTANCE apart)
			const bool near_ally = ((is_neighbour || shares_threat) && close(instance_manager, defines, country, other))
				|| (is_great_power(defines, other) && cached_interest(instance_manager, other, country) >= fixed_point_t::_0_25);
			if (!near_ally) {
				continue;
			}
			fixed_point_t score = power_of(other) * (fixed_point_t::_1 + fixed_point_t(std::max(0, static_cast<std::int32_t>(country.get_relations_with(other)))) / 100);
			if (shares_threat) {
				score *= 2;
			}
			if (best == nullptr || score > best_score) {
				best = &other;
				best_score = score;
			}
		}
		return best;
	}

	// The processes of the relations the country wants: (the other country, +1 / -1), the most important first
	void wanted_processes(
		InstanceManager& instance_manager, OpenVicDiplomacyDefines const& defines, CountryInstance& country,
		std::vector<std::vector<CountryInstance*>> const& neighbours, std::vector<std::pair<CountryInstance*, std::int8_t>>& out
	) {
		const auto add = [&out](CountryInstance* other, const std::int8_t direction) {
			if (other == nullptr) {
				return;
			}
			for (auto const& [existing, unused] : out) {
				if (existing == other) {
					return;
				}
			}
			out.emplace_back(other, direction);
		};
		std::vector<CountryInstance*> const& own_neighbours = neighbours[type_safe::get(country.index)];
		// A war of conquest: the relation with the target goes down
		if (country.get_ai_goal() == ai_goal_t::CONQUEST && country.get_ai_goal_target() != nullptr
			&& country.get_relations_with(*country.get_ai_goal_target()) > defines.ai_conquest_worsen_to) {
			add(const_cast<CountryInstance*>(country.get_ai_goal_target()), -1);
		}
		// An ally against the biggest threat: the relation with the best candidate goes up to make the alliance possible
		if (CountryInstance* const threat_country = biggest_threat(instance_manager, defines, country, own_neighbours)) {
			if (instance_manager.get_country_relation_manager().get_alliance_count(&country) < static_cast<std::size_t>(std::max(0, defines.ai_max_alliances))) {
				CountryInstance* const candidate = alliance_candidate(instance_manager, defines, country, *threat_country, neighbours, 0);
				if (candidate != nullptr && country.get_relations_with(*candidate) < defines.ai_alliance_min_relation) {
					add(candidate, 1);
				}
			}
		}
		// Economic control: the small countries of the highest interest
		if (country.get_ai_goal() == ai_goal_t::ECONOMIC_CONTROL) {
			std::vector<std::pair<fixed_point_t, CountryInstance*>> small;
			for (CountryInstance& other : instance_manager.get_country_instance_manager().get_country_instances()) {
				if (&other == &country || !is_active(other) || !is_small(defines, other) || country.is_at_war_with(other)
					|| country.get_relations_with(other) >= defines.relation_max) {
					continue;
				}
				const fixed_point_t value = DiplomacyAI::interest(instance_manager, country, other);
				if (value > fixed_point_t::_0) {
					small.emplace_back(value, &other);
				}
			}
			std::stable_sort(small.begin(), small.end(), [](auto const& a, auto const& b) { return a.first > b.first; });
			for (std::size_t i = 0; i < small.size() && static_cast<std::int32_t>(i) < defines.ai_control_targets; ++i) {
				add(small[i].second, 1);
			}
		}
	}

	// The goal of a great power (every AI_GOAL_YEARS): by the situation and a dice
	void choose_goal(
		InstanceManager& instance_manager, OpenVicDiplomacyDefines const& defines, CountryInstance& country,
		std::vector<std::vector<CountryInstance*>> const& neighbours
	) {
		const Date today = instance_manager.get_today();
		std::vector<CountryInstance*> const& own_neighbours = neighbours[type_safe::get(country.index)];
		fixed_point_t threat_value = fixed_point_t::_0;
		CountryInstance* const threat_country = biggest_threat(instance_manager, defines, country, own_neighbours, &threat_value);

		// The best target of a conquest: a weak land neighbour that we do not like
		CountryInstance* conquest_target = nullptr;
		fixed_point_t conquest_score = fixed_point_t::_0;
		for (CountryInstance* other : own_neighbours) {
			if (!is_active(*other) || country.has_alliance_with(*other) || country.get_relations_with(*other) >= defines.ai_war_max_relation
				|| power_of(*other) > defines.ai_conquest_max_power.of(power_of(country))) {
				continue;
			}
			const fixed_point_t score = DiplomacyAI::war_target_score(instance_manager, country, *other);
			if (score > conquest_score) {
				conquest_score = score;
				conquest_target = other;
			}
		}
		std::int32_t small_neighbours = 0;
		for (CountryInstance* other : own_neighbours) {
			if (is_active(*other) && is_small(defines, *other)) {
				++small_neighbours;
			}
		}
		const std::size_t alliances = instance_manager.get_country_relation_manager().get_alliance_count(&country);

		// The weights of the goals, x 0.8..1.2 by the dice (a little of the unexpected)
		std::array<fixed_point_t, 5> weight {};
		weight[static_cast<std::size_t>(ai_goal_t::STRENGTHEN_ARMY)] = fixed_point_t::_1 + threat_value * 2;
		// the conquest: the score of the target counts up to 1 (a target ten times weaker than us is not ten times more tempting)
		weight[static_cast<std::size_t>(ai_goal_t::CONQUEST)] = conquest_target != nullptr
			? fixed_point_t::_1 + std::min(fixed_point_t::_1, conquest_score / 10) : fixed_point_t::_0;
		weight[static_cast<std::size_t>(ai_goal_t::ECONOMIC_CONTROL)] = small_neighbours > 0 ? fixed_point_t::_1 + fixed_point_t(small_neighbours) / 5 : fixed_point_t::_0_50;
		weight[static_cast<std::size_t>(ai_goal_t::COALITION)] = threat_country != nullptr && alliances < static_cast<std::size_t>(std::max(0, defines.ai_max_alliances))
			? fixed_point_t::_1 + threat_value * 2 : fixed_point_t::_0_25;
		ai_goal_t goal = ai_goal_t::STRENGTHEN_ARMY;
		fixed_point_t best = fixed_point_t::_0;
		for (std::size_t index = 1; index < weight.size(); ++index) {
			const fixed_point_t value = weight[index] * (80 + dice(country, today, 0x601 + index) * 40 / 100) / 100;
			if (value > best) {
				best = value;
				goal = static_cast<ai_goal_t>(index);
			}
		}
		CountryInstance* target = goal == ai_goal_t::CONQUEST ? conquest_target : goal == ai_goal_t::COALITION ? threat_country : nullptr;
		country.set_ai_goal(goal);
		country.set_ai_goal_target(target);
		country.set_ai_goal_until(today + Timespan::from_years(std::max(1, defines.ai_goal_years)));
		static constexpr std::array<std::string_view, 5> GOAL_KEYS { "none", "strengthen_army", "conquest", "economic_control", "coalition" };
		instance_manager.get_notification_log().add(today, &country, "ai_goal", GOAL_KEYS[static_cast<std::size_t>(goal)], target);
		SPDLOG_INFO("AI goal: {} chooses {} (target {})", country.get_identifier(), GOAL_KEYS[static_cast<std::size_t>(goal)],
			target != nullptr ? target->get_identifier() : std::string_view { "-" });
	}
}

fixed_point_t DiplomacyAI::interest(InstanceManager const& instance_manager, CountryInstance const& country, CountryInstance const& other) {
	if (&country == &other || other.get_capital() == nullptr) {
		return fixed_point_t::_0;
	}
	OpenVicDiplomacyDefines const& defines = defines_of(instance_manager);
	MapInstance const& map_instance = instance_manager.get_map_instance();
	MapDefinition const& map_definition = static_cast<MapDefinition const&>(map_instance);
	// A land neighbour: 1
	for (ProvinceInstance const* province : country.get_owned_provinces()) {
		for (ProvinceDefinition::adjacency_t const& adjacency : province->province_definition.get_adjacencies()) {
			if (adjacency.get_type() == ProvinceDefinition::adjacency_t::type_t::LAND && !adjacency.get_to().is_water()
				&& map_instance.get_province_instance_by_definition(adjacency.get_to()).get_owner() == &other) {
				return fixed_point_t::_1;
			}
		}
	}
	// By the sea: the closeness of the nearest state of ours x the share of our navy in the strongest one
	fixed_point_t strongest_navy = fixed_point_t::_1;
	for (CountryInstance const& candidate : instance_manager.get_country_instance_manager().get_country_instances()) {
		strongest_navy = std::max(strongest_navy, candidate.get_military_power_from_sea_untracked());
	}
	const fixed_point_t navy_share = std::clamp(country.get_military_power_from_sea_untracked() / strongest_navy, fixed_point_t::_0, fixed_point_t::_1);
	if (navy_share <= fixed_point_t::_0) {
		return fixed_point_t::_0;
	}
	fixed_point_t nearest = fixed_point_t(std::max(1, defines.ai_interest_distance));
	for (State const* state : country.get_states()) {
		if (state == nullptr || state->get_capital() == nullptr) {
			continue;
		}
		nearest = std::min(nearest, map_definition.calculate_distance_between(state->get_capital()->province_definition, other.get_capital()->province_definition));
	}
	const fixed_point_t closeness = fixed_point_t::_1 - nearest / std::max(1, defines.ai_interest_distance);
	return std::clamp(closeness, fixed_point_t::_0, fixed_point_t::_1) * navy_share;
}

fixed_point_t DiplomacyAI::war_target_score(InstanceManager const& instance_manager, CountryInstance const& country, CountryInstance const& target) {
	OpenVicDiplomacyDefines const& defines = defines_of(instance_manager);
	const std::int32_t relation = country.get_relations_with(target);
	if (country.has_alliance_with(target) || relation >= defines.ai_war_max_relation) {
		return fixed_point_t::_0;
	}
	// one sphere (version 92, docs/PLAN_SPHERES.md): the members and the leader do not attack each other
	CountryInstance const* const own_sphere = country.get_sphere_owner_untracked() != nullptr ? country.get_sphere_owner_untracked()
		: country.get_leads_sphere() ? &country : nullptr;
	CountryInstance const* const target_sphere = target.get_sphere_owner_untracked() != nullptr ? target.get_sphere_owner_untracked()
		: target.get_leads_sphere() ? &target : nullptr;
	if (own_sphere != nullptr && own_sphere == target_sphere) {
		return fixed_point_t::_0;
	}
	// weaker is better, a bad relation is better, the target of the goal much better, a great power is a risk
	fixed_point_t score = power_of(country) / power_of(target);
	score *= fixed_point_t::_1 + fixed_point_t(std::max(0, -relation)) / 100;
	if (country.get_ai_goal() == ai_goal_t::CONQUEST && country.get_ai_goal_target() == &target) {
		score *= 3;
	}
	if (is_great_power(defines, target)) {
		score /= 2;
	}
	return score;
}

std::int32_t DiplomacyAI::alliance_acceptance(InstanceManager const& instance_manager, CountryInstance const& proposer, CountryInstance const& receiver) {
	OpenVicDiplomacyDefines const& defines = defines_of(instance_manager);
	CountryRelationManager const& relations = instance_manager.get_country_relation_manager();
	if (proposer.is_at_war() || receiver.is_at_war_with(proposer)
		|| relations.get_alliance_count(&receiver) >= static_cast<std::size_t>(std::max(0, defines.ai_max_alliances))
		|| receiver.get_relations_with(proposer) < defines.ai_alliance_accept_relation) {
		return 0;
	}
	// it is worth it: the proposer is not much weaker than us, or it is a great power
	if (power_of(proposer) * 4 < power_of(receiver) && !is_great_power(defines, proposer)) {
		return 0;
	}
	return 1;
}

void DiplomacyAI::monthly_update(InstanceManager& instance_manager) {
	OpenVicDiplomacyDefines const& defines = defines_of(instance_manager);
	if (defines.ai_diplomacy_enabled == 0) {
		return;
	}
	const Date today = instance_manager.get_today();
	CountryRelationManager& relations = instance_manager.get_country_relation_manager();
	const std::vector<std::vector<CountryInstance*>> neighbours = land_neighbours(instance_manager);
	std::unordered_map<std::uint64_t, fixed_point_t> month_interest;
	interest_cache = &month_interest;
	struct reset_cache_t {
		~reset_cache_t() {
			interest_cache = nullptr;
		}
	} reset_cache;

	for (CountryInstance& country : instance_manager.get_country_instance_manager().get_country_instances()) {
		if (!is_active(country) || !country.is_ai()) {
			continue;
		}
		std::vector<CountryInstance*> const& own_neighbours = neighbours[type_safe::get(country.index)];

		// The goal of a great power: a new one every AI_GOAL_YEARS, or when the target is gone
		if (is_great_power(defines, country)) {
			CountryInstance const* const target = country.get_ai_goal_target();
			if (country.get_ai_goal() == ai_goal_t::NONE || today >= country.get_ai_goal_until() || (target != nullptr && !is_active(*target))) {
				choose_goal(instance_manager, defines, country, neighbours);
			}
		} else if (country.get_ai_goal() != ai_goal_t::NONE) {
			country.set_ai_goal(ai_goal_t::NONE);
			country.set_ai_goal_target(nullptr);
		}

		// An ally against the biggest threat (at peace, under the limit of the alliances)
		if (!country.is_at_war() && relations.get_alliance_count(&country) < static_cast<std::size_t>(std::max(0, defines.ai_max_alliances))) {
			if (CountryInstance* const threat_country = biggest_threat(instance_manager, defines, country, own_neighbours)) {
				CountryInstance* const candidate = alliance_candidate(
					instance_manager, defines, country, *threat_country, neighbours, defines.ai_alliance_min_relation
				);
				// the player answers his proposals himself (not yet: the AI does not propose to the player)
				if (candidate != nullptr && candidate->is_ai() && alliance_acceptance(instance_manager, country, *candidate) > 0) {
					country.set_alliance_with(*candidate, true);
					if (is_great_power(defines, country) || is_great_power(defines, *candidate)) {
						instance_manager.get_notification_log().add(today, &country, "alliance_formed", "", candidate);
					}
					SPDLOG_INFO("AI: {} allies with {} against {}", country.get_identifier(), candidate->get_identifier(), threat_country->get_identifier());
				}
			}
		}

		// The processes of the relations: the wanted ones, at most AI_MAX_RELATION_PROCESSES; the others stop
		std::vector<std::pair<CountryInstance*, std::int8_t>> wanted;
		wanted_processes(instance_manager, defines, country, neighbours, wanted);
		if (static_cast<std::int32_t>(wanted.size()) > defines.ai_max_relation_processes) {
			wanted.resize(static_cast<std::size_t>(std::max(0, defines.ai_max_relation_processes)));
		}
		for (auto const& [receiver, direction] : relations.get_relation_processes_of(&country)) {
			const bool keep = std::any_of(wanted.begin(), wanted.end(), [receiver, direction](auto const& w) {
				return w.first == receiver && w.second == direction;
			});
			if (!keep) {
				relations.set_relation_process(&country, receiver, 0);
			}
		}
		for (auto const& [receiver, direction] : wanted) {
			if (relations.get_relation_process(&country, receiver) != direction && !country.is_at_war_with(*receiver)) {
				relations.set_relation_process(&country, receiver, direction);
			}
		}
	}
}

void DiplomacyAI::weekly_assist(InstanceManager& instance_manager) {
	MapInstance& map_instance = instance_manager.get_map_instance();
	const Date today = instance_manager.get_today();
	for (CountryInstance& country : instance_manager.get_country_instance_manager().get_country_instances()) {
		if (!country.get_ai_assist() || country.is_ai() || !is_active(country)) {
			continue;
		}
		// The goods: what the government needs and has not, and does not buy, is bought again (the automatic stockpile)
		for (GoodInstance const& good_instance : instance_manager.get_good_instance_manager().get_good_instances()) {
			CountryInstance::good_data_t& good_data = country.get_good_data(good_instance);
			if (good_data.is_automated) {
				continue;
			}
			const bool short_of_it = good_data.government_needs > good_data.stockpile_amount;
			const bool buys = !good_data.is_selling && good_data.stockpile_cutoff > good_data.stockpile_amount;
			if (short_of_it && !buys) {
				good_data.is_automated = true;
				instance_manager.get_notification_log().add(today, &country, "ai_assist_goods", good_instance.get_identifier());
			}
		}

		// The war: the borders with an enemy that has no army of ours next to them (once per enemy and war)
		memory::vector<CountryInstance const*> warned = country.get_ai_assist_warned_borders();
		std::erase_if(warned, [&country](CountryInstance const* enemy) { return !country.is_at_war_with(*enemy); });
		if (country.is_at_war()) {
			std::vector<ProvinceInstance const*> border; // our provinces next to the enemy
			std::vector<CountryInstance const*> enemies;
			for (ProvinceInstance const* province : country.get_owned_provinces()) {
				for (ProvinceDefinition::adjacency_t const& adjacency : province->province_definition.get_adjacencies()) {
					if (adjacency.get_type() != ProvinceDefinition::adjacency_t::type_t::LAND || adjacency.get_to().is_water()) {
						continue;
					}
					CountryInstance const* const owner = map_instance.get_province_instance_by_definition(adjacency.get_to()).get_owner();
					if (owner != nullptr && owner != &country && country.is_at_war_with(*owner)) {
						border.push_back(province);
						if (std::find(enemies.begin(), enemies.end(), owner) == enemies.end()) {
							enemies.push_back(owner);
						}
					}
				}
			}
			for (CountryInstance const* enemy : enemies) {
				if (std::find(warned.begin(), warned.end(), enemy) != warned.end()) {
					continue;
				}
				bool guarded = false;
				for (ArmyInstance const& army : instance_manager.get_unit_instance_manager().get_armies()) {
					if (&army.get_country() != &country) {
						continue;
					}
					ProvinceInstance const& location = army.get_location();
					if (location.get_owner() == enemy) {
						guarded = true;
						break;
					}
					for (ProvinceInstance const* province : border) {
						if (province == &location) {
							guarded = true;
							break;
						}
					}
					if (guarded) {
						break;
					}
				}
				if (!guarded) {
					instance_manager.get_notification_log().add(today, &country, "border_undefended", "", enemy);
					warned.push_back(enemy);
				}
			}
		}
		country.set_ai_assist_warned_borders(std::move(warned));
	}
}

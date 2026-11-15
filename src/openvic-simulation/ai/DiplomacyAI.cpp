#include "DiplomacyAI.hpp"

#include <algorithm>
#include <array>
#include <optional>
#include <unordered_map>
#include <vector>

#include "openvic-simulation/DefinitionManager.hpp"
#include "openvic-simulation/InstanceManager.hpp"
#include "openvic-simulation/country/CountryInstance.hpp"
#include "openvic-simulation/country/CountryInstanceManager.hpp"
#include "openvic-simulation/diagnostics/TickProfiler.hpp"
#include "openvic-simulation/diplomacy/CountryRelation.hpp"
#include "openvic-simulation/economy/GoodInstance.hpp"
#include "openvic-simulation/map/MapDefinition.hpp"
#include "openvic-simulation/map/MapInstance.hpp"
#include "openvic-simulation/map/ProvinceInstance.hpp"
#include "openvic-simulation/map/State.hpp"
#include "openvic-simulation/military/UnitInstanceGroup.hpp"
#include "openvic-simulation/military/War.hpp"
#include "openvic-simulation/diplomacy/Subjects.hpp"
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
	// What DiplomacyAI::interest needs of all the countries, worked out once for the run of monthly_update (nothing it reads changes
	// during the run: the owners of the provinces and the navies): the land neighbours and the strongest navy (nullptr outside of the run)
	std::vector<std::vector<CountryInstance*>> const* month_neighbours = nullptr;
	fixed_point_t month_strongest_navy = fixed_point_t::_0;

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

	// The number of the alliances of a country: CountryRelationManager::get_alliance_count goes through all the pairs of alliances, and the
	// AI asked it for every pair of countries. During monthly_update the counts are kept here and the counts of the two countries of a new
	// alliance are worked out again (nullptr outside of the run)
	std::vector<std::size_t>* month_alliance_counts = nullptr;

	std::size_t alliance_count(CountryRelationManager const& relations, CountryInstance const* country) {
		if (month_alliance_counts != nullptr) {
			const std::size_t index = static_cast<std::size_t>(type_safe::get(country->index));
			if (index < month_alliance_counts->size()) {
				return (*month_alliance_counts)[index];
			}
		}
		return relations.get_alliance_count(country);
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

	// How dangerous the other is: its power over ours x (150 - the relation) / 150 (a third to twice), less when it is far.
	// Without the alliance (an ally is no threat: threat() checks it) and, when shared_cache is false, without the cache of the interest
	// (the table of the threats is worked out in parallel)
	fixed_point_t threat_unless_allied(
		InstanceManager const& instance_manager, OpenVicDiplomacyDefines const& defines, CountryInstance const& country, CountryInstance const& other,
		const bool neighbour, const bool shared_cache
	) {
		if (&country == &other) {
			return fixed_point_t::_0;
		}
		if (!neighbour && !is_great_power(defines, other)) {
			return fixed_point_t::_0;
		}
		const std::int32_t relation = country.get_relations_with(other);
		const fixed_point_t relation_factor = std::clamp(fixed_point_t(150 - relation) / 150, fixed_point_t::_1 / 3, fixed_point_t(2));
		fixed_point_t value = power_of(other) / power_of(country) * relation_factor;
		// an infamous country (more infamy than BADBOY_LIMIT, version 98) is feared more
		if (other.is_infamous()) {
			value = value * (100 + defines.ai_infamous_threat_pct) / 100;
		}
		if (!neighbour) {
			// a great power that is not a neighbour threatens as much as the country interests it (Argentina does not fear Russia)
			value = defines.ai_far_threat.of(value)
				* (shared_cache ? cached_interest(instance_manager, other, country) : DiplomacyAI::interest(instance_manager, other, country));
		}
		return value;
	}

	fixed_point_t threat(
		InstanceManager const& instance_manager, OpenVicDiplomacyDefines const& defines, CountryInstance const& country, CountryInstance const& other,
		const bool neighbour
	) {
		if (&country == &other || country.has_alliance_with(other)) {
			return fixed_point_t::_0;
		}
		return threat_unless_allied(instance_manager, defines, country, other, neighbour, true);
	}

	// The threats of the other countries to every AI country without the alliances ([country index * count + other index]), worked out in
	// parallel at the start of monthly_update: nothing they read changes during its run but the alliances, which biggest_threat checks
	// itself. month_threat_rows: whether the row of a country was worked out (nullptr outside of the run)
	std::vector<fixed_point_t>* month_threats = nullptr;
	std::vector<char>* month_threat_rows = nullptr;
	std::size_t month_threat_count = 0;

	// The biggest threat of the country among its neighbours and the great powers (nullptr: none above AI_THREAT_MIN)
	CountryInstance* biggest_threat(
		InstanceManager& instance_manager, OpenVicDiplomacyDefines const& defines, CountryInstance const& country,
		std::vector<CountryInstance*> const& neighbours, fixed_point_t* value_out = nullptr
	) {
		CountryInstance* best = nullptr;
		fixed_point_t best_value = defines.ai_threat_min.fixed();
		const std::size_t row = static_cast<std::size_t>(type_safe::get(country.index));
		if (month_threats != nullptr && row < month_threat_count && (*month_threat_rows)[row] != 0) {
			// the table of the month: the same values as threat(), the alliance is checked here (only for a value that would win)
			fixed_point_t const* const values = month_threats->data() + row * month_threat_count;
			std::size_t index = 0;
			for (CountryInstance& other : instance_manager.get_country_instance_manager().get_country_instances()) {
				const std::size_t other_index = index++;
				if (!is_active(other)) {
					continue;
				}
				fixed_point_t value = values[other_index];
				if (value > best_value && country.has_alliance_with(other)) {
					value = fixed_point_t::_0;
				}
				if (value > best_value) {
					best_value = value;
					best = &other;
				}
			}
		} else {
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
				|| alliance_count(relations, &other) >= static_cast<std::size_t>(std::max(0, defines.ai_max_alliances))) {
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
		// The subjects (version 101): their relation goes down every month; it is kept at AI_OVERLORD_KEEP_RELATION, a neighbour is brought up
		// to the annexation
		{
			CountryRelationManager const& relations = instance_manager.get_country_relation_manager();
			for (CountryInstance& subject : instance_manager.get_country_instance_manager().get_country_instances()) {
				if (subject.overlord != &country || !is_active(subject)) {
					continue;
				}
				const std::int32_t wanted = contains(own_neighbours, &subject)
					? defines.subject_annex_min_relation - Subjects::get_relation_bonus(defines, subject.subject_type) : defines.ai_overlord_keep_relation;
				if (relations.get_country_relation(&country, &subject) < std::min(wanted, defines.relation_max)) {
					add(&subject, 1);
				}
			}
		}
		// A war of conquest: the relation with the target goes down
		if (country.get_ai_goal() == ai_goal_t::CONQUEST && country.get_ai_goal_target() != nullptr
			&& country.get_relations_with(*country.get_ai_goal_target()) > defines.ai_conquest_worsen_to) {
			add(const_cast<CountryInstance*>(country.get_ai_goal_target()), -1);
		}
		// An ally against the biggest threat: the relation with the best candidate goes up to make the alliance possible
		if (CountryInstance* const threat_country = biggest_threat(instance_manager, defines, country, own_neighbours)) {
			if (alliance_count(instance_manager.get_country_relation_manager(), &country) < static_cast<std::size_t>(std::max(0, defines.ai_max_alliances))) {
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
		const std::size_t alliances = alliance_count(instance_manager.get_country_relation_manager(), &country);

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
	if (month_neighbours != nullptr) {
		if (contains((*month_neighbours)[type_safe::get(country.index)], &other)) {
			return fixed_point_t::_1;
		}
	} else {
		for (ProvinceInstance const* province : country.get_owned_provinces()) {
			for (ProvinceDefinition::adjacency_t const& adjacency : province->province_definition.get_adjacencies()) {
				if (adjacency.get_type() == ProvinceDefinition::adjacency_t::type_t::LAND && !adjacency.get_to().is_water()
					&& map_instance.get_province_instance_by_definition(adjacency.get_to()).get_owner() == &other) {
					return fixed_point_t::_1;
				}
			}
		}
	}
	// By the sea: the closeness of the nearest state of ours x the share of our navy in the strongest one
	fixed_point_t strongest_navy = fixed_point_t::_1;
	if (month_neighbours != nullptr) {
		strongest_navy = month_strongest_navy;
	} else {
		for (CountryInstance const& candidate : instance_manager.get_country_instance_manager().get_country_instances()) {
			strongest_navy = std::max(strongest_navy, candidate.get_military_power_from_sea_untracked());
		}
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

namespace {
	// The countries fight on the same side of a war
	bool same_side(WarManager const& war_manager, CountryInstance const& country, CountryInstance const& other) {
		for (War const& war : war_manager.get_wars()) {
			if (war.has_participant(country) && war.has_participant(other) && war.is_attacker(country) == war.is_attacker(other)) {
				return true;
			}
		}
		return false;
	}

	// The biggest size of subsidies to the receiver (50..150) that costs at most the budget a day, 0 if none
	std::int32_t subsidy_level_within(
		InstanceManager const& instance_manager, CountryInstance const& receiver, const fixed_point_t budget
	) {
		const fixed_point_t base = instance_manager.definition_manager.get_define_manager().get_diplomacy_defines().get_warsubsidies_percent()
			* receiver.get_actual_military_spending().load();
		if (base <= fixed_point_t::_0 || budget <= fixed_point_t::_0) {
			return 0;
		}
		for (auto it = CountryRelationManager::WAR_SUBSIDY_LEVELS.rbegin(); it != CountryRelationManager::WAR_SUBSIDY_LEVELS.rend(); ++it) {
			if (base * *it / 100 <= budget) {
				return *it;
			}
		}
		return 0;
	}
}

void DiplomacyAI::ai_war_subsidies(
	InstanceManager& instance_manager, OpenVicDiplomacyDefines const& defines, CountryInstance& country, const Date today
) {
	CountryRelationManager& relations = instance_manager.get_country_relation_manager();
	WarManager const& war_manager = instance_manager.get_war_manager();
	const fixed_point_t balance = country.get_balance_before_investment_average();
	const fixed_point_t income = std::max(country.get_income_yesterday(), fixed_point_t::_0);
	const fixed_point_t cash = country.get_cash_stockpile().load();
	// what it can give a day: a share of its surplus (never with a deficit)
	fixed_point_t budget = balance > fixed_point_t::_0 ? balance * defines.ai_subsidy_surplus_pct / 100 : fixed_point_t::_0;
	const bool can_pay = balance > fixed_point_t::_0 && cash >= income * defines.ai_subsidy_stop_cash_days;
	// what it pays: stopped with a deficit, the size follows the surplus
	for (CountryInstance& receiver : instance_manager.get_country_instance_manager().get_country_instances()) {
		if (&receiver == &country || !country.is_sending_war_subsidy_to(receiver)) {
			continue;
		}
		// (the size stays, it only goes down when the surplus no longer pays it)
		const std::int32_t affordable = can_pay ? subsidy_level_within(instance_manager, receiver, budget) : 0;
		if (affordable == 0) {
			country.set_sending_war_subsidy_to(receiver, false);
			continue;
		}
		const std::int32_t level = std::min(affordable, relations.get_war_subsidy_level(&country, &receiver));
		relations.set_war_subsidy_level(&country, &receiver, level);
		budget -= instance_manager.definition_manager.get_define_manager().get_diplomacy_defines().get_warsubsidies_percent()
			* receiver.get_actual_military_spending().load() * level / 100;
	}
	// a new one, sometimes: to a poor country at war, its ally while it is at peace or a smaller country of its side of a war
	if (!can_pay || cash < income * defines.ai_subsidy_start_cash_days || dice(country, today, 0x5b5) >= defines.ai_subsidy_chance_pct) {
		return;
	}
	for (CountryInstance& receiver : instance_manager.get_country_instance_manager().get_country_instances()) {
		if (&receiver == &country || !receiver.exists() || !receiver.is_at_war() || country.is_at_war_with(receiver)
			|| country.is_sending_war_subsidy_to(receiver)) {
			continue;
		}
		const bool ally_at_war = country.has_alliance_with(receiver) && !country.is_at_war();
		const bool smaller_partner = power_of(country) > power_of(receiver) && same_side(war_manager, country, receiver);
		if (!ally_at_war && !smaller_partner) {
			continue;
		}
		const fixed_point_t receiver_income = std::max(receiver.get_income_yesterday(), fixed_point_t::_0);
		const bool poor = receiver.get_cash_stockpile().load() < receiver_income * defines.ai_subsidy_poor_days
			|| receiver.get_balance_before_investment_average() < fixed_point_t::_0;
		if (!poor) {
			continue;
		}
		// a size it can pay, chosen by the dice: an AI gives much or little
		const std::int32_t affordable = subsidy_level_within(instance_manager, receiver, budget);
		if (affordable == 0) {
			continue;
		}
		const std::int32_t choices = static_cast<std::int32_t>(std::count_if(
			CountryRelationManager::WAR_SUBSIDY_LEVELS.begin(), CountryRelationManager::WAR_SUBSIDY_LEVELS.end(),
			[affordable](const std::int32_t candidate) -> bool { return candidate <= affordable; }
		));
		const std::int32_t level = CountryRelationManager::WAR_SUBSIDY_LEVELS[static_cast<std::size_t>(dice(country, today, 0x5b6) % choices)];
		relations.set_war_subsidy_level(&country, &receiver, level);
		SPDLOG_INFO("AI: {} pays war subsidies of {} % to {}", country.get_identifier(), level, receiver.get_identifier());
		if (receiver.is_ai()) {
			country.set_sending_war_subsidy_to(receiver, true);
		} else {
			// the country of a player answers it (the proposals, version 96)
			DiplomaticActionManager const& actions = instance_manager.definition_manager.get_diplomatic_action_manager();
			if (const std::optional<std::size_t> index = actions.get_action_index("war_subsidies"); index.has_value()) {
				actions.propose(*index, &country, &receiver, instance_manager);
			}
		}
		return;
	}
}

void DiplomacyAI::ai_military_access(
	InstanceManager& instance_manager, OpenVicDiplomacyDefines const& defines, CountryInstance& country,
	std::vector<CountryInstance*> const& own_neighbours, std::vector<std::vector<CountryInstance*>> const& neighbours
) {
	if (!country.is_at_war()) {
		return;
	}
	// a neighbour (not an enemy, not a partner whose land is open anyway) that borders an enemy: one request a month
	for (CountryInstance* const neighbour : own_neighbours) {
		if (neighbour == nullptr || country.is_at_war_with(*neighbour) || country.has_military_access_to(*neighbour)
			|| country.is_military_partner(*neighbour)) {
			continue;
		}
		std::vector<CountryInstance*> const& theirs = neighbours[type_safe::get(neighbour->index)];
		const bool borders_enemy = std::any_of(theirs.begin(), theirs.end(), [&country](CountryInstance const* other) -> bool {
			return other != nullptr && country.is_at_war_with(*other);
		});
		if (!borders_enemy) {
			continue;
		}
		if (neighbour->is_ai()) {
			if (military_access_acceptance(instance_manager, country, *neighbour) > 0) {
				instance_manager.get_country_relation_manager().set_has_military_access_to(&country, neighbour, true);
				SPDLOG_INFO("AI: {} gets military access through {}", country.get_identifier(), neighbour->get_identifier());
			}
		} else {
			DiplomaticActionManager const& actions = instance_manager.definition_manager.get_diplomatic_action_manager();
			if (const std::optional<std::size_t> index = actions.get_action_index("request_military_access"); index.has_value()) {
				actions.propose(*index, &country, neighbour, instance_manager);
			}
		}
		return;
	}
}

void DiplomacyAI::ai_subjects(InstanceManager& instance_manager, OpenVicDiplomacyDefines const& defines, CountryInstance& country, const Date today) {
	CountryRelationManager const& relations = instance_manager.get_country_relation_manager();
	DiplomaticActionManager const& actions = instance_manager.definition_manager.get_diplomatic_action_manager();
	// A subject: the war of independence when the overlord is weak and not liked
	if (CountryInstance* const overlord = country.overlord; overlord != nullptr) {
		if (Subjects::may_attack_overlord(country.subject_type) && !country.is_at_war()
			&& (today - country.subject_since).to_int() >= defines.ai_subject_independence_min_years * 365
			&& relations.get_effective_relation(&country, overlord) < defines.ai_subject_independence_max_relation
			&& dice(country, today, 0x6d1) < defines.ai_subject_independence_chance) {
			// the power of the overlord that would fight: half while it fights another war
			fixed_point_t overlord_power = power_of(*overlord);
			if (overlord->is_at_war()) {
				overlord_power /= 2;
			}
			if (power_of(country) * 100 >= overlord_power * defines.ai_subject_independence_power_pct
				&& instance_manager.get_war_manager().declare_war_as_country(
					instance_manager, country, *overlord, WarGoal { WarGoal::type_t::INDEPENDENCE, nullptr }
				)) {
				SPDLOG_INFO("AI: {} fights for its independence from {}", country.get_identifier(), overlord->get_identifier());
			}
		}
		return;
	}
	// An overlord: its subjects (in the order of the countries)
	const auto act = [&actions, &instance_manager, &country](std::string_view identifier, CountryInstance& subject) -> bool {
		const std::optional<std::size_t> index = actions.get_action_index(identifier);
		return index.has_value() && actions.execute_diplomatic_action(*index, &country, &subject, {}, instance_manager);
	};
	std::uint64_t salt = 0x6e0;
	for (CountryInstance& subject : instance_manager.get_country_instance_manager().get_country_instances()) {
		if (subject.overlord != &country || !is_active(subject)) {
			continue;
		}
		salt++;
		if (Subjects::get_annex_block_reason(instance_manager, country, subject).empty() && dice(country, today, salt) < defines.ai_overlord_annex_chance) {
			if (act("annex_subject", subject)) {
				SPDLOG_INFO("AI: {} annexes its subject {}", country.get_identifier(), subject.get_identifier());
			}
			continue;
		}
		if (!Subjects::tribute_is_changeable(subject.subject_type)) {
			continue;
		}
		// the payments: more with a deficit while the subject likes us, less when it does not
		const std::int32_t relation = relations.get_effective_relation(&country, &subject);
		if (country.get_balance_before_investment_average() < fixed_point_t::_0 && relation >= defines.ai_overlord_keep_relation
			&& subject.subject_tribute_pct + defines.subject_tribute_step_pct <= Subjects::get_max_tribute_pct(defines, subject.subject_type)) {
			act("raise_subject_tribute", subject);
		} else if (relation < 0 && subject.subject_tribute_pct - defines.subject_tribute_step_pct >= Subjects::get_min_tribute_pct(defines, subject.subject_type)) {
			act("lower_subject_tribute", subject);
		}
		// the coercion: into the war of the overlord
		if (country.is_at_war() && !subject.is_at_war() && dice(country, today, salt + 0x100) < defines.ai_overlord_coerce_chance) {
			if (act("subject_join_war", subject)) {
				SPDLOG_INFO("AI: {} draws its subject {} into its war", country.get_identifier(), subject.get_identifier());
			}
		}
	}
}

std::int32_t DiplomacyAI::military_access_acceptance(
	InstanceManager const& instance_manager, CountryInstance const& asker, CountryInstance const& giver
) {
	if (giver.is_at_war_with(asker)) {
		return 0;
	}
	// never to a country at war with an ally
	for (CountryInstance const& country : instance_manager.get_country_instance_manager().get_country_instances()) {
		if (&country != &asker && &country != &giver && giver.has_alliance_with(country) && asker.is_at_war_with(country)) {
			return 0;
		}
	}
	if (giver.has_alliance_with(asker) || giver.shares_sphere_with(asker)) {
		return 1;
	}
	CountryRelationManager const& relations = instance_manager.get_country_relation_manager();
	const std::int32_t minimum = instance_manager.definition_manager.get_define_manager().get_openvic_defines().diplomacy.ai_military_access_min_relation;
	return relations.get_effective_relation(&giver, &asker) >= minimum ? 1 : 0;
}

std::int32_t DiplomacyAI::alliance_acceptance(InstanceManager const& instance_manager, CountryInstance const& proposer, CountryInstance const& receiver) {
	OpenVicDiplomacyDefines const& defines = defines_of(instance_manager);
	CountryRelationManager const& relations = instance_manager.get_country_relation_manager();
	// (a puppet has no foreign policy, version 101)
	if (proposer.is_puppet() || receiver.is_puppet()) {
		return 0;
	}
	if (proposer.is_at_war() || receiver.is_at_war_with(proposer)
		|| alliance_count(relations, &receiver) >= static_cast<std::size_t>(std::max(0, defines.ai_max_alliances))
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
	TickProfiler& profiler = instance_manager.get_tick_profiler();
	std::optional<TickProfiler::Scope> section;
	section.emplace(profiler.scope("neighbours"));
	const std::vector<std::vector<CountryInstance*>> neighbours = land_neighbours(instance_manager);
	section.reset();
	std::unordered_map<std::uint64_t, fixed_point_t> month_interest;
	interest_cache = &month_interest;
	month_neighbours = &neighbours;
	month_strongest_navy = fixed_point_t::_1;
	for (CountryInstance const& candidate : instance_manager.get_country_instance_manager().get_country_instances()) {
		month_strongest_navy = std::max(month_strongest_navy, candidate.get_military_power_from_sea_untracked());
	}
	std::vector<std::size_t> alliance_counts;
	for (CountryInstance const& candidate : instance_manager.get_country_instance_manager().get_country_instances()) {
		alliance_counts.push_back(relations.get_alliance_count(&candidate));
	}
	month_alliance_counts = &alliance_counts;

	// The table of the threats, in parallel (see month_threats). The military power is worked out when it is first asked for: here, on one
	// thread, before the threads read it
	CountryInstanceManager& country_instance_manager = instance_manager.get_country_instance_manager();
	const std::size_t country_count = country_instance_manager.get_country_instances().size();
	for (CountryInstance const& candidate : country_instance_manager.get_country_instances()) {
		(void) power_of(candidate);
	}
	std::vector<fixed_point_t> threats(country_count * country_count, fixed_point_t::_0);
	std::vector<char> threat_rows(country_count, 0);
	{
		section.emplace(profiler.scope("threats"));
		std::vector<CountryInstance*> all_countries;
		for (CountryInstance& candidate : country_instance_manager.get_country_instances()) {
			all_countries.push_back(&candidate);
		}
		country_instance_manager.for_each_country_parallel([&](CountryInstance& country) -> void {
			if (!is_active(country) || !country.is_ai()) {
				return;
			}
			const std::size_t row = static_cast<std::size_t>(type_safe::get(country.index));
			if (row >= country_count) {
				return;
			}
			std::vector<CountryInstance*> const& own_neighbours = neighbours[row];
			fixed_point_t* const values = threats.data() + row * country_count;
			for (std::size_t other_index = 0; other_index < country_count; ++other_index) {
				CountryInstance const& other = *all_countries[other_index];
				if (!is_active(other)) {
					continue;
				}
				const bool neighbour = contains(own_neighbours, &other) && close(instance_manager, defines, country, other);
				values[other_index] = threat_unless_allied(instance_manager, defines, country, other, neighbour, false);
			}
			threat_rows[row] = 1;
		});
		section.reset();
	}
	month_threats = &threats;
	month_threat_rows = &threat_rows;
	month_threat_count = country_count;
	struct reset_cache_t {
		~reset_cache_t() {
			interest_cache = nullptr;
			month_neighbours = nullptr;
			month_alliance_counts = nullptr;
			month_threats = nullptr;
			month_threat_rows = nullptr;
		}
	} reset_cache;

	for (CountryInstance& country : instance_manager.get_country_instance_manager().get_country_instances()) {
		if (!is_active(country) || !country.is_ai()) {
			continue;
		}
		std::vector<CountryInstance*> const& own_neighbours = neighbours[type_safe::get(country.index)];

		// The goal of a great power: a new one every AI_GOAL_YEARS, or when the target is gone
		section.emplace(profiler.scope("goals"));
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
		section.reset();
		section.emplace(profiler.scope("alliances"));
		if (!country.is_at_war() && alliance_count(relations, &country) < static_cast<std::size_t>(std::max(0, defines.ai_max_alliances))) {
			if (CountryInstance* const threat_country = biggest_threat(instance_manager, defines, country, own_neighbours)) {
				CountryInstance* const candidate = alliance_candidate(
					instance_manager, defines, country, *threat_country, neighbours, defines.ai_alliance_min_relation
				);
				// the country of a player answers a proposal itself (version 96: a proposal waits for its answer); the AI proposes only the
				// alliances that it would accept in the place of the player
				if (candidate != nullptr && !candidate->is_ai()) {
					DiplomaticActionManager const& actions = instance_manager.definition_manager.get_diplomatic_action_manager();
					const std::optional<std::size_t> index = actions.get_action_index("form_alliance");
					if (index.has_value() && alliance_acceptance(instance_manager, country, *candidate) > 0) {
						actions.propose(*index, &country, candidate, instance_manager);
					}
				} else if (candidate != nullptr && alliance_acceptance(instance_manager, country, *candidate) > 0) {
					country.set_alliance_with(*candidate, true);
					alliance_counts[static_cast<std::size_t>(type_safe::get(country.index))] = relations.get_alliance_count(&country);
					alliance_counts[static_cast<std::size_t>(type_safe::get(candidate->index))] = relations.get_alliance_count(candidate);
					if (is_great_power(defines, country) || is_great_power(defines, *candidate)) {
						instance_manager.get_notification_log().add(today, &country, "alliance_formed", "", candidate);
					}
					SPDLOG_INFO("AI: {} allies with {} against {}", country.get_identifier(), candidate->get_identifier(), threat_country->get_identifier());
				}
			}
		}

		// The subjects (version 101)
		section.reset();
		section.emplace(profiler.scope("subjects"));
		ai_subjects(instance_manager, defines, country, today);
		if (!is_active(country)) {
			continue;
		}

		// The war subsidies (version 99, the owner 2026-11-15) and the military access (version 99)
		section.reset();
		section.emplace(profiler.scope("subsidies and access"));
		ai_war_subsidies(instance_manager, defines, country, today);
		ai_military_access(instance_manager, defines, country, own_neighbours, neighbours);

		// The processes of the relations: the wanted ones, at most AI_MAX_RELATION_PROCESSES; the others stop
		std::vector<std::pair<CountryInstance*, std::int8_t>> wanted;
		section.reset();
		section.emplace(profiler.scope("processes"));
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

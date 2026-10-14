#include "War.hpp"

#include <algorithm>
#include <unordered_map>
#include <unordered_set>

#include "openvic-simulation/DefinitionManager.hpp"
#include "openvic-simulation/InstanceManager.hpp"
#include "openvic-simulation/country/CountryInstance.hpp"
#include "openvic-simulation/economy/BuildingInstance.hpp"
#include "openvic-simulation/economy/BuildingType.hpp"
#include "openvic-simulation/map/MapInstance.hpp"
#include "openvic-simulation/map/ProvinceInstance.hpp"
#include "openvic-simulation/map/State.hpp"
#include "openvic-simulation/modifier/ModifierEffectCache.hpp"
#include "openvic-simulation/military/LeaderTrait.hpp"
#include "openvic-simulation/military/UnitInstanceGroup.hpp"
#include "openvic-simulation/utility/Logger.hpp"

using namespace OpenVic;

namespace {
	bool contains_country(memory::vector<CountryInstance*> const& countries, CountryInstance const* country) {
		return std::find(countries.begin(), countries.end(), country) != countries.end();
	}

	// Armies of the province copied to a plain vector, so that armies can be moved or destroyed while it is processed
	std::vector<ArmyInstance*> copy_armies(ProvinceInstance& province) {
		std::vector<ArmyInstance*> result;
		for (ArmyInstance& army : province.get_armies()) {
			result.push_back(&army);
		}
		return result;
	}

	// Armies of the province that are not on board of a ship, copied to a plain vector
	std::vector<ArmyInstance*> copy_active_armies(ProvinceInstance& province) {
		std::vector<ArmyInstance*> result;
		for (ArmyInstance& army : province.get_armies()) {
			if (army.get_transport_navy() == nullptr) {
				result.push_back(&army);
			}
		}
		return result;
	}

	// Bonuses of a general from the personality and background traits
	struct LeaderBonus {
		fixed_point_t attack, defence, morale, attrition;
	};

	LeaderBonus get_leader_bonus(ArmyInstance const& army, ModifierEffectCache const& cache) {
		LeaderBonus bonus;
		LeaderInstance const* const leader = army.get_leader();
		if (leader == nullptr) {
			return bonus;
		}
		for (LeaderTrait const* trait : { leader->get_personality(), leader->get_background() }) {
			if (trait == nullptr) {
				continue;
			}
			if (cache.get_attack_leader() != nullptr) {
				bonus.attack += trait->get_effect(*cache.get_attack_leader());
			}
			if (cache.get_defence_leader() != nullptr) {
				bonus.defence += trait->get_effect(*cache.get_defence_leader());
			}
			if (cache.get_morale_leader() != nullptr) {
				bonus.morale += trait->get_effect(*cache.get_morale_leader());
			}
			if (cache.get_attrition_leader() != nullptr) {
				bonus.attrition += trait->get_effect(*cache.get_attrition_leader());
			}
		}
		return bonus;
	}

	struct BattleSide {
		std::vector<ArmyInstance*> armies;
		fixed_point_t power;
		// Divides the damage the side takes: defence of the regiments, generals and the dig in of the armies
		fixed_point_t defence_factor = fixed_point_t::_1;
		// Divides the organisation the side loses: morale of the generals
		fixed_point_t morale_factor = fixed_point_t::_1;
		fixed_point_t organisation;
		fixed_point_t max_organisation;
		size_t regiment_count = 0;

		fixed_point_t get_organisation_ratio() const {
			return max_organisation > fixed_point_t::_0 ? organisation / max_organisation : fixed_point_t::_0;
		}
	};

	// Each point of a general's attack / defence / morale is worth this share
	constexpr int32_t LEADER_POINT_DIVISOR = 10;
	// Each level of dig in makes the army take this much less damage
	constexpr int32_t DIG_IN_DIVISOR = 10;
	constexpr std::uint64_t LEADER_DEATH_ODDS = 30;

	void calculate_side(BattleSide& side, ModifierEffectCache const& cache) {
		const fixed_point_t min_org_factor = fixed_point_t(3) / 10;

		side.power = fixed_point_t::_0;
		side.organisation = fixed_point_t::_0;
		side.max_organisation = fixed_point_t::_0;
		side.regiment_count = 0;

		fixed_point_t defence_sum = fixed_point_t::_0;
		fixed_point_t morale_sum = fixed_point_t::_0;
		fixed_point_t dig_in_best = fixed_point_t::_0;

		for (ArmyInstance* army : side.armies) {
			const LeaderBonus bonus = get_leader_bonus(*army, cache);
			const fixed_point_t attack_multiplier = std::max(fixed_point_t::_0_25, fixed_point_t::_1 + bonus.attack / LEADER_POINT_DIVISOR);
			const fixed_point_t defence_multiplier = std::max(fixed_point_t::_0_25, fixed_point_t::_1 + bonus.defence / LEADER_POINT_DIVISOR);
			const fixed_point_t morale_multiplier = std::max(fixed_point_t::_0_25, fixed_point_t::_1 + bonus.morale);

			if (fixed_point_t(static_cast<int32_t>(army->get_dig_in_level())) > dig_in_best) {
				dig_in_best = fixed_point_t(static_cast<int32_t>(army->get_dig_in_level()));
			}

			for (RegimentInstance& regiment : army->get_regiment_instances()) {
				const fixed_point_t max_strength = regiment.get_max_strength();
				const fixed_point_t max_organisation = regiment.get_max_organisation();
				if (max_strength <= fixed_point_t::_0 || regiment.get_strength() <= fixed_point_t::_0) {
					continue;
				}

				const fixed_point_t strength_factor = regiment.get_strength() / max_strength;
				fixed_point_t organisation_factor = max_organisation > fixed_point_t::_0
					? regiment.get_organisation() / max_organisation
					: fixed_point_t::_0;
				if (organisation_factor < min_org_factor) {
					organisation_factor = min_org_factor;
				}

				side.power += (regiment.get_regiment_type().attack + fixed_point_t::_1) * strength_factor * organisation_factor
					* attack_multiplier;
				defence_sum += (regiment.get_regiment_type().defence + fixed_point_t::_1) * defence_multiplier;
				morale_sum += morale_multiplier;
				side.organisation += regiment.get_organisation();
				side.max_organisation += max_organisation;
				side.regiment_count++;
			}
		}

		if (side.regiment_count > 0) {
			const int32_t count = static_cast<int32_t>(side.regiment_count);
			// A regiment with defence 1 (value 2 here) takes the damage of the earlier model, better defence takes less
			side.defence_factor = std::max(fixed_point_t::_0_50, defence_sum / count / 2)
				* (fixed_point_t::_1 + dig_in_best / DIG_IN_DIVISOR);
			side.morale_factor = morale_sum / count;
		}
	}

	// Each regiment of the target takes the same share of the damage
	void apply_damage(BattleSide& target, fixed_point_t enemy_power) {
		if (target.regiment_count == 0) {
			return;
		}

		const fixed_point_t organisation_damage = fixed_point_t(5) / 100;
		const fixed_point_t strength_damage = fixed_point_t(1) / 100;

		const fixed_point_t average_power = enemy_power / static_cast<int32_t>(target.regiment_count);
		const fixed_point_t strength_power = average_power / target.defence_factor;
		const fixed_point_t organisation_power = strength_power / target.morale_factor;

		for (ArmyInstance* army : target.armies) {
			for (RegimentInstance& regiment : army->get_regiment_instances()) {
				if (regiment.get_strength() <= fixed_point_t::_0) {
					continue;
				}
				// Regiments without organisation are routing and lose men much faster
				const int32_t rout_factor = regiment.get_organisation() > fixed_point_t::_0 ? 1 : 3;
				regiment.take_damage(
					regiment.get_max_organisation() * organisation_power * organisation_damage,
					regiment.get_max_strength() * strength_power * strength_damage * rout_factor
				);
			}
		}
	}

	// A province next to `from` the army can retreat to, nullptr if there is none
	ProvinceInstance* find_retreat_province(ProvinceInstance const& from, ArmyInstance const& army, MapInstance& map_instance) {
		CountryInstance const& country = army.get_country();
		ProvinceInstance* fallback = nullptr;

		for (ProvinceDefinition::adjacency_t const& adjacency : from.province_definition.get_adjacencies()) {
			if (adjacency.get_type() != ProvinceDefinition::adjacency_t::type_t::LAND || adjacency.get_to().is_water()) {
				continue;
			}
			ProvinceInstance& candidate = map_instance.get_province_instance_by_definition(adjacency.get_to());
			if (!UnitInstanceGroup::can_country_enter(country, candidate)) {
				continue;
			}

			bool enemy_present = false;
			for (ArmyInstance const& other : candidate.get_armies()) {
				if (other.get_country().is_at_war_with(country)) {
					enemy_present = true;
					break;
				}
			}
			if (enemy_present) {
				continue;
			}

			CountryInstance const* const owner = candidate.get_owner();
			if (owner == &country || (owner != nullptr && country.has_alliance_with(*owner))) {
				return &candidate;
			}
			if (fallback == nullptr) {
				fallback = &candidate;
			}
		}

		return fallback;
	}
}

bool War::is_attacker(CountryInstance const& country) const {
	return contains_country(attackers, &country);
}

bool War::is_defender(CountryInstance const& country) const {
	return contains_country(defenders, &country);
}

War* WarManager::find_war(const std::uint32_t id) {
	for (War& war : wars) {
		if (war.id == id) {
			return &war;
		}
	}
	return nullptr;
}

War const* WarManager::get_war_by_id(const std::uint32_t id) const {
	for (War const& war : wars) {
		if (war.id == id) {
			return &war;
		}
	}
	return nullptr;
}

War* WarManager::find_war_between(CountryInstance const& first, CountryInstance const& second) {
	for (War& war : wars) {
		if ((war.is_attacker(first) && war.is_defender(second)) || (war.is_defender(first) && war.is_attacker(second))) {
			return &war;
		}
	}
	return nullptr;
}

memory::vector<War const*> WarManager::get_wars_of(CountryInstance const& country) const {
	memory::vector<War const*> result;
	for (War const& war : wars) {
		if (war.has_participant(country)) {
			result.push_back(&war);
		}
	}
	return result;
}

bool WarManager::has_truce(CountryInstance const& first, CountryInstance const& second, const Date today) const {
	for (Truce const& truce : truces) {
		if (truce.until > today
			&& ((truce.first == &first && truce.second == &second) || (truce.first == &second && truce.second == &first))) {
			return true;
		}
	}
	return false;
}

WarManager::Siege const* WarManager::get_siege(ProvinceInstance const& province) const {
	const auto it = sieges.find(&province);
	return it != sieges.end() ? &it->second : nullptr;
}

std::int32_t WarManager::get_province_weight(ProvinceInstance const& province) {
	CountryInstance const* const owner = province.get_owner();
	if (owner != nullptr && owner->get_capital() == &province) {
		return CAPITAL_PROVINCE_WEIGHT;
	}
	return 1;
}

std::int32_t WarManager::get_siege_days_required(ProvinceInstance const& province) {
	std::int32_t fort_levels = 0;
	for (BuildingInstance const& building : province.get_buildings()) {
		fort_levels += static_cast<std::int32_t>(type_safe::get(building.get_level()))
			* static_cast<std::int32_t>(type_safe::get(building.building_type.fort_level));
	}
	return SIEGE_BASE_DAYS + fort_levels * SIEGE_DAYS_PER_FORT_LEVEL;
}

bool WarManager::can_declare_war(CountryInstance const& attacker, CountryInstance const& defender, const Date today) const {
	if (&attacker == &defender || !attacker.exists() || !defender.exists()) {
		return false;
	}
	if (attacker.is_rebel_country() || defender.is_rebel_country()) {
		return false;
	}
	if (attacker.is_at_war_with(defender) || attacker.has_alliance_with(defender)) {
		return false;
	}
	return !has_truce(attacker, defender, today);
}

bool WarManager::declare_war(InstanceManager& instance_manager, CountryInstance& attacker, CountryInstance& defender) {
	return declare_war(instance_manager, attacker, defender, WarGoal {});
}

bool WarManager::declare_war(
	InstanceManager& instance_manager, CountryInstance& attacker, CountryInstance& defender, WarGoal const& goal
) {
	if (!can_declare_war(attacker, defender, instance_manager.get_today())) {
		return false;
	}
	// The goal state must belong to the defender
	if (goal.type == WarGoal::type_t::CONQUER_STATE
		&& (goal.state_province == nullptr || goal.state_province->get_owner() != &defender)) {
		return false;
	}

	War war;
	war.goal = goal;
	war.id = next_war_id++;
	war.start_date = instance_manager.get_today();
	war.attackers.push_back(&attacker);
	war.defenders.push_back(&defender);

	// Allies join the side of their ally, unless they are allied to both sides or already fight their ally
	for (CountryInstance& country : instance_manager.get_country_instance_manager().get_country_instances()) {
		if (&country == &attacker || &country == &defender || !country.exists() || country.is_rebel_country()) {
			continue;
		}

		const bool allied_to_attacker = country.has_alliance_with(attacker);
		const bool allied_to_defender = country.has_alliance_with(defender);
		if (allied_to_attacker == allied_to_defender) {
			continue;
		}

		if (allied_to_attacker) {
			if (!country.is_at_war_with(attacker)) {
				war.attackers.push_back(&country);
			}
		} else if (!country.is_at_war_with(defender)) {
			war.defenders.push_back(&country);
		}
	}

	for (CountryInstance* attacking : war.attackers) {
		for (CountryInstance* defending : war.defenders) {
			attacking->set_at_war_with(*defending, true);
		}
	}

	instance_manager.get_notification_log().add(instance_manager.get_today(), &attacker, "war_declared", "", &defender);
	SPDLOG_INFO(
		"War {} started: {} (+{} allies) attacks {} (+{} allies)",
		war.id, attacker.get_identifier(), war.attackers.size() - 1, defender.get_identifier(), war.defenders.size() - 1
	);

	wars.push_back(std::move(war));
	return true;
}

bool WarManager::would_accept_peace(
	InstanceManager const& instance_manager, War const& war, const bool proposer_is_attacker, const peace_type_t peace_type
) const {
	const std::int32_t days = (instance_manager.get_today() - war.start_date).to_int();
	const fixed_point_t proposer_score = war.get_score(proposer_is_attacker);
	const fixed_point_t other_score = war.get_score(!proposer_is_attacker);

	switch (peace_type) {
	case peace_type_t::WHITE_PEACE:
		// The side that is clearly winning refuses, unless the war has dragged on for years
		return other_score - proposer_score < 15 || days > 3 * 365;
	case peace_type_t::TAKE_STATES: {
		if (proposer_score >= 40) {
			return true;
		}
		// Surrender when the capital of the leader has fallen
		memory::vector<CountryInstance*> const& other_side = war.get_side(!proposer_is_attacker);
		memory::vector<CountryInstance*> const& proposer_side = war.get_side(proposer_is_attacker);
		if (!other_side.empty()) {
			CountryInstance const* const leader = other_side.front();
			ProvinceInstance const* const capital = leader->get_capital();
			if (capital != nullptr && proposer_score >= 25 && contains_country(proposer_side, capital->get_controller())) {
				return true;
			}
		}
		return days > 5 * 365 && proposer_score >= 20;
	}
	case peace_type_t::GIVE_STATES:
		return true;
	}

	return false;
}

bool WarManager::propose_peace(
	InstanceManager& instance_manager, const std::uint32_t war_id, CountryInstance& proposer, const peace_type_t peace_type
) {
	War const* const war = find_war(war_id);
	if (war == nullptr || !war->has_participant(proposer)) {
		return false;
	}

	const bool proposer_is_attacker = war->is_attacker(proposer);

	if (!would_accept_peace(instance_manager, *war, proposer_is_attacker, peace_type)) {
		return false;
	}

	switch (peace_type) {
	case peace_type_t::WHITE_PEACE:
		break;
	case peace_type_t::TAKE_STATES:
		transfer_states(instance_manager, *war, proposer_is_attacker);
		break;
	case peace_type_t::GIVE_STATES:
		transfer_states(instance_manager, *war, !proposer_is_attacker);
		break;
	}

	end_war(instance_manager, war_id);
	return true;
}

void WarManager::transfer_states(InstanceManager& instance_manager, War const& war, const bool winners_are_attackers) {
	memory::vector<CountryInstance*> const& winners = war.get_side(winners_are_attackers);
	memory::vector<CountryInstance*> const& losers = war.get_side(!winners_are_attackers);

	struct Candidate {
		State* state;
		CountryInstance* old_owner;
		CountryInstance* recipient;
		fixed_point_t cost;
	};
	std::vector<Candidate> candidates;

	for (CountryInstance* loser : losers) {
		std::int32_t total_weight = 0;
		for (ProvinceInstance const* province : loser->get_owned_provinces()) {
			total_weight += get_province_weight(*province);
		}
		if (total_weight <= 0) {
			continue;
		}

		std::unordered_set<State*> seen_states;
		for (ProvinceInstance* province : loser->get_owned_provinces()) {
			State* const state = province->get_state();
			if (state == nullptr || !seen_states.insert(state).second) {
				continue;
			}

			// The state can be taken if the winners hold at least one of its provinces
			CountryInstance* recipient = nullptr;
			std::int32_t state_weight = 0;
			for (ProvinceInstance& state_province : state->get_provinces()) {
				state_weight += get_province_weight(state_province);
				CountryInstance* const controller = state_province.get_controller();
				if (recipient == nullptr && controller != loser && contains_country(winners, controller)) {
					recipient = controller;
				}
			}
			if (recipient == nullptr) {
				continue;
			}

			candidates.push_back({ state, loser, recipient, fixed_point_t(state_weight) / total_weight * 100 });
		}
	}

	// Cheap states first so that the score is spent on as many states as possible
	std::stable_sort(candidates.begin(), candidates.end(), [](Candidate const& lhs, Candidate const& rhs) -> bool {
		return lhs.cost < rhs.cost;
	});

	fixed_point_t budget = war.get_score(winners_are_attackers);
	std::unordered_set<CountryInstance*> changed_countries;

	for (Candidate const& candidate : candidates) {
		if (candidate.cost > budget) {
			continue;
		}
		budget -= candidate.cost;

		for (ProvinceInstance& province : candidate.state->get_provinces()) {
			province.set_owner(candidate.recipient);
			province.set_controller(candidate.recipient);
		}
		changed_countries.insert(candidate.old_owner);
	}

	// A country that lost its capital moves it to one of its remaining provinces
	for (CountryInstance* country : changed_countries) {
		ProvinceInstance* const capital = country->get_capital();
		if (capital != nullptr && capital->get_owner() == country) {
			continue;
		}
		ProvinceInstance* new_capital = nullptr;
		for (ProvinceInstance* province : country->get_owned_provinces()) {
			new_capital = province;
			break;
		}
		country->set_capital(new_capital);
	}
}

void WarManager::end_war(InstanceManager& instance_manager, const std::uint32_t war_id) {
	const auto war_it = std::find_if(wars.begin(), wars.end(), [war_id](War const& war) -> bool {
		return war.id == war_id;
	});
	if (war_it == wars.end()) {
		return;
	}

	const War war = *war_it;
	wars.erase(war_it);

	const Date today = instance_manager.get_today();

	for (CountryInstance* attacker : war.attackers) {
		for (CountryInstance* defender : war.defenders) {
			truces.push_back({ attacker, defender, today + Timespan::from_days(TRUCE_DAYS) });
			// Still enemies if another war keeps them apart
			if (find_war_between(*attacker, *defender) == nullptr) {
				attacker->set_at_war_with(*defender, false);
			}
		}
	}

	truces.erase(
		std::remove_if(truces.begin(), truces.end(), [today](Truce const& truce) -> bool {
			return truce.until <= today;
		}),
		truces.end()
	);

	SPDLOG_INFO("War {} ended", war_id);
	if (!war.attackers.empty() && !war.defenders.empty()) {
		instance_manager.get_notification_log().add(today, war.attackers.front(), "peace_signed", "", war.defenders.front());
	}

	MapInstance& map_instance = instance_manager.get_map_instance();

	// Occupied provinces return to their owners unless the occupier still fights the owner
	for (ProvinceInstance& province : map_instance.get_province_instances()) {
		CountryInstance* const owner = province.get_owner();
		CountryInstance* const controller = province.get_controller();
		if (owner != nullptr && controller != nullptr && controller != owner && !controller->is_at_war_with(*owner)) {
			province.set_controller(owner);
		}
	}

	// Armies that no longer have the right to stand in a province go home
	for (ProvinceInstance& province : map_instance.get_province_instances()) {
		if (province.get_armies().empty()) {
			continue;
		}
		for (ArmyInstance* army : copy_armies(province)) {
			CountryInstance& country = army->get_country();
			if (UnitInstanceGroup::can_country_enter(country, province)) {
				continue;
			}

			ProvinceInstance* home = country.get_capital();
			if (home == nullptr || home->get_owner() != &country) {
				home = nullptr;
				for (ProvinceInstance* owned : country.get_owned_provinces()) {
					home = owned;
					break;
				}
			}

			army->clear_path();
			if (home != nullptr) {
				army->set_location(*home);
			} else {
				instance_manager.get_unit_instance_manager().destroy_group(*army);
			}
		}
	}
}

void WarManager::update_combat(InstanceManager& instance_manager) {
	MapInstance& map_instance = instance_manager.get_map_instance();
	UnitInstanceManager& unit_manager = instance_manager.get_unit_instance_manager();
	ModifierEffectCache const& cache = instance_manager.definition_manager.get_modifier_manager().get_modifier_effect_cache();

	for (ProvinceInstance& province : map_instance.get_province_instances()) {
		if (province.get_armies().empty() || province.province_definition.is_water()) {
			continue;
		}

		std::vector<ArmyInstance*> armies = copy_active_armies(province);
		for (ArmyInstance* army : armies) {
			army->set_in_combat(false);
		}
		if (armies.size() < 2) {
			continue;
		}

		// Find a country that has an enemy in the province, the battle is fought between its side and the enemies
		CountryInstance const* lead_country = nullptr;
		for (size_t i = 0; i < armies.size() && lead_country == nullptr; ++i) {
			for (size_t j = i + 1; j < armies.size(); ++j) {
				if (armies[i]->get_country().is_at_war_with(armies[j]->get_country())) {
					lead_country = &armies[i]->get_country();
					break;
				}
			}
		}
		if (lead_country == nullptr) {
			continue;
		}

		BattleSide side_a, side_b;
		for (ArmyInstance* army : armies) {
			if (army->empty()) {
				continue;
			}
			CountryInstance const& country = army->get_country();
			if (country.is_at_war_with(*lead_country)) {
				side_b.armies.push_back(army);
			}
		}
		for (ArmyInstance* army : armies) {
			if (army->empty()) {
				continue;
			}
			CountryInstance const& country = army->get_country();
			if (&country == lead_country) {
				side_a.armies.push_back(army);
				continue;
			}
			if (country.is_at_war_with(*lead_country)) {
				continue;
			}
			// An ally fights alongside if it is at war with someone on the other side
			const bool fights_b = std::any_of(side_b.armies.begin(), side_b.armies.end(), [&country](ArmyInstance* enemy) -> bool {
				return country.is_at_war_with(enemy->get_country());
			});
			if (fights_b) {
				side_a.armies.push_back(army);
			}
		}

		calculate_side(side_a, cache);
		calculate_side(side_b, cache);
		if (side_a.regiment_count == 0 || side_b.regiment_count == 0) {
			continue;
		}

		const fixed_point_t power_a = side_a.power;
		const fixed_point_t power_b = side_b.power;
		const size_t regiments_a = side_a.regiment_count;
		const size_t regiments_b = side_b.regiment_count;

		apply_damage(side_b, power_a);
		apply_damage(side_a, power_b);
		calculate_side(side_a, cache);
		calculate_side(side_b, cache);

		for (ArmyInstance* army : side_a.armies) {
			fought_today.insert(army->unique_id);
		}
		for (ArmyInstance* army : side_b.armies) {
			fought_today.insert(army->unique_id);
		}

		const fixed_point_t rout_threshold = fixed_point_t(1) / 10;
		const fixed_point_t ratio_a = side_a.get_organisation_ratio();
		const fixed_point_t ratio_b = side_b.get_organisation_ratio();
		const bool routed_a = side_a.regiment_count == 0 || ratio_a < rout_threshold;
		const bool routed_b = side_b.regiment_count == 0 || ratio_b < rout_threshold;

		if (!routed_a && !routed_b) {
			for (ArmyInstance* army : side_a.armies) {
				army->set_in_combat(true);
			}
			for (ArmyInstance* army : side_b.armies) {
				army->set_in_combat(true);
			}
			continue;
		}

		const bool a_loses = routed_a && (!routed_b || ratio_a <= ratio_b);
		BattleSide& loser = a_loses ? side_a : side_b;
		BattleSide& winner = a_loses ? side_b : side_a;
		const size_t loser_regiments = a_loses ? regiments_a : regiments_b;

		// Battle score for the winner's side
		if (!loser.armies.empty() && !winner.armies.empty()) {
			CountryInstance const& winner_country = winner.armies.front()->get_country();
			CountryInstance const& loser_country = loser.armies.front()->get_country();
			if (War* const war = find_war_between(winner_country, loser_country); war != nullptr) {
				const fixed_point_t gain = fixed_point_t::_1 + fixed_point_t(static_cast<int32_t>(std::min<size_t>(loser_regiments, 20))) / 4;
				fixed_point_t& score = war->is_attacker(winner_country) ? war->attacker_battle_score : war->defender_battle_score;
				score += gain;
				if (score > MAX_BATTLE_SCORE) {
					score = fixed_point_t(MAX_BATTLE_SCORE);
				}
			}
		}

		// The generals of the winners gain prestige, the generals of the losers lose it and may die
		for (ArmyInstance* army : winner.armies) {
			if (LeaderInstance* const leader = army->get_leader()) {
				leader->add_prestige(fixed_point_t(1) / 100);
			}
		}
		for (ArmyInstance* army : loser.armies) {
			if (LeaderInstance* const leader = army->get_leader()) {
				leader->add_prestige(fixed_point_t(-1) / 100);
				// A deterministic "dice": about one lost battle in LEADER_DEATH_ODDS kills the general
				const std::uint64_t dice = (leader->unique_id * 2654435761ULL + static_cast<std::uint64_t>((instance_manager.get_today() - Date {}).to_int()) * 40503ULL) >> 3;
				if (dice % LEADER_DEATH_ODDS == 0) {
					unit_manager.destroy_leader(*leader);
				}
			}
		}

		// The losers retreat, or are destroyed if there is nowhere to go
		for (ArmyInstance* army : loser.armies) {
			army->clear_path();
			ProvinceInstance* const retreat = find_retreat_province(province, *army, map_instance);
			if (retreat != nullptr) {
				army->set_location(*retreat);
			} else {
				unit_manager.destroy_group(*army);
			}
		}
	}
}

void WarManager::update_sieges(InstanceManager& instance_manager) {
	std::unordered_set<ProvinceInstance const*> active;

	for (ProvinceInstance& province : instance_manager.get_map_instance().get_province_instances()) {
		if (province.province_definition.is_water() || province.get_armies().empty()) {
			continue;
		}

		CountryInstance* const controller = province.get_controller();
		if (controller == nullptr) {
			continue;
		}

		CountryInstance* besieger = nullptr;
		bool contested = false;
		for (ArmyInstance& army : province.get_armies()) {
			if (army.get_transport_navy() != nullptr) {
				continue;
			}
			if (army.is_in_combat()) {
				contested = true;
				break;
			}
			if (besieger == nullptr && army.get_country().is_at_war_with(*controller)) {
				besieger = &army.get_country();
			}
		}
		if (besieger == nullptr || contested) {
			continue;
		}
		// Enemy armies of the besieger in the province stop the siege
		for (ArmyInstance const& army : province.get_armies()) {
			if (army.get_transport_navy() == nullptr && army.get_country().is_at_war_with(*besieger)) {
				contested = true;
				break;
			}
		}
		if (contested) {
			continue;
		}

		active.insert(&province);
		Siege& siege = sieges[&province];
		if (siege.besieger != besieger) {
			siege = {};
			siege.besieger = besieger;
		}
		siege.days_required = get_siege_days_required(province);
		// More regiments take a fortress faster, the siege skill of the regiments adds to it (up to 4 times as fast)
		fixed_point_t siege_speed = fixed_point_t::_1;
		for (ArmyInstance const& army : province.get_armies()) {
			if (&army.get_country() != besieger || army.get_transport_navy() != nullptr) {
				continue;
			}
			for (RegimentInstance const& regiment : army.get_regiment_instances()) {
				siege_speed += fixed_point_t(1) / 8 + regiment.get_regiment_type().siege / 10;
			}
		}
		siege_speed = std::min(siege_speed, fixed_point_t(4));
		siege.progress += siege_speed;
		siege.days = siege.progress.floor<int32_t>();

		if (siege.days >= siege.days_required) {
			province.set_controller(besieger);
			active.erase(&province);
		}
	}

	for (auto it = sieges.begin(); it != sieges.end();) {
		if (active.contains(it->first)) {
			++it;
		} else {
			it = sieges.erase(it);
		}
	}
}

void WarManager::update_scores(InstanceManager& instance_manager) {
	for (War& war : wars) {
		// Share of the enemies' land (by weight) that the side holds, 0..100
		const auto occupation_score = [](
			memory::vector<CountryInstance*> const& owners, memory::vector<CountryInstance*> const& occupiers
		) -> fixed_point_t {
			std::int32_t total = 0;
			std::int32_t held = 0;
			for (CountryInstance const* owner : owners) {
				for (ProvinceInstance const* province : owner->get_owned_provinces()) {
					const std::int32_t weight = get_province_weight(*province);
					total += weight;
					if (province->get_controller() != owner && contains_country(occupiers, province->get_controller())) {
						held += weight;
					}
				}
			}
			return total > 0 ? fixed_point_t(held) / total * 100 : fixed_point_t::_0;
		};

		war.attacker_score = std::min<fixed_point_t>(
			occupation_score(war.defenders, war.attackers) + war.attacker_battle_score, fixed_point_t(100)
		);
		war.defender_score = std::min<fixed_point_t>(
			occupation_score(war.attackers, war.defenders) + war.defender_battle_score, fixed_point_t(100)
		);
	}
}

void WarManager::update_army_recovery(InstanceManager& instance_manager) {
	const fixed_point_t organisation_recovery = fixed_point_t(2) / 100;
	const fixed_point_t strength_recovery = fixed_point_t(5) / 1000;

	for (ProvinceInstance& province : instance_manager.get_map_instance().get_province_instances()) {
		for (ArmyInstance& army : province.get_armies()) {
			if (fought_today.contains(army.unique_id)) {
				continue;
			}

			// Men are only replaced on friendly ground
			CountryInstance const* const owner = province.get_owner();
			const bool friendly = owner != nullptr
				&& (owner == &army.get_country() || army.get_country().has_alliance_with(*owner));

			// Armies the national stockpile cannot supply recover more slowly, and get no new men below half supplies
			// (only at war, and never below a quarter, so that a country with an empty stockpile still can recover)
			fixed_point_t supply = fixed_point_t::_1;
			if (army.get_country().is_at_war()) {
				supply = std::max(army.get_country().get_army_supply_ratio(), fixed_point_t::_0_25);
			}
			for (RegimentInstance& regiment : army.get_regiment_instances()) {
				regiment.recover(
					regiment.get_max_organisation() * organisation_recovery * supply,
					friendly && supply >= fixed_point_t::_0_50 ? regiment.get_max_strength() * strength_recovery : fixed_point_t::_0
				);
			}
		}
	}
}

namespace {
	struct NavalSide {
		std::vector<NavyInstance*> navies;
		fixed_point_t power;
		fixed_point_t organisation;
		fixed_point_t max_organisation;
		size_t ship_count = 0;

		fixed_point_t get_organisation_ratio() const {
			return max_organisation > fixed_point_t::_0 ? organisation / max_organisation : fixed_point_t::_0;
		}
	};

	// Share of the maximum strength / organisation of a ship that is lost each day by a ship hit with the force
	// of its own hull
	constexpr int32_t NAVAL_STRENGTH_DAMAGE_PERCENT = 2;
	constexpr int32_t NAVAL_ORGANISATION_DAMAGE_PERCENT = 8;

	void calculate_naval_side(NavalSide& side) {
		const fixed_point_t min_org_factor = fixed_point_t(3) / 10;
		side.power = fixed_point_t::_0;
		side.organisation = fixed_point_t::_0;
		side.max_organisation = fixed_point_t::_0;
		side.ship_count = 0;

		for (NavyInstance* navy : side.navies) {
			for (ShipInstance& ship : navy->get_ship_instances()) {
				if (ship.get_max_strength() <= fixed_point_t::_0 || ship.get_strength() <= fixed_point_t::_0) {
					continue;
				}
				const fixed_point_t strength_factor = ship.get_strength() / ship.get_max_strength();
				fixed_point_t organisation_factor = ship.get_max_organisation() > fixed_point_t::_0
					? ship.get_organisation() / ship.get_max_organisation() : fixed_point_t::_0;
				if (organisation_factor < min_org_factor) {
					organisation_factor = min_org_factor;
				}
				side.power += (ship.get_ship_type().gun_power + ship.get_ship_type().torpedo_attack) * strength_factor * organisation_factor;
				side.organisation += ship.get_organisation();
				side.max_organisation += ship.get_max_organisation();
				side.ship_count++;
			}
		}
	}

	void apply_naval_damage(NavalSide& target, const fixed_point_t enemy_power) {
		if (target.ship_count == 0) {
			return;
		}
		const fixed_point_t average_power = enemy_power / static_cast<int32_t>(target.ship_count);

		for (NavyInstance* navy : target.navies) {
			for (ShipInstance& ship : navy->get_ship_instances()) {
				if (ship.get_strength() <= fixed_point_t::_0) {
					continue;
				}
				// A thin hull takes the full force, a thick one a part of it (at most 6 times the damage); evasion avoids some hits
				const fixed_point_t hull = std::max(ship.get_ship_type().hull, fixed_point_t::_1);
				fixed_point_t ratio = average_power / hull / (fixed_point_t::_1 + ship.get_ship_type().evasion / 10);
				ratio = std::min(ratio, fixed_point_t(6));
				const int32_t rout_factor = ship.get_organisation() > fixed_point_t::_0 ? 1 : 3;
				ship.take_damage(
					ship.get_max_organisation() * ratio * NAVAL_ORGANISATION_DAMAGE_PERCENT / 100,
					ship.get_max_strength() * ratio * NAVAL_STRENGTH_DAMAGE_PERCENT * rout_factor / 100
				);
			}
		}
	}

	// The fleet flees to the nearest port it can dock in, with the armies on board; it is lost if there is none
	void retreat_navy(NavyInstance& navy, UnitInstanceManager& unit_manager, MapInstance& map_instance) {
		navy.clear_path();
		ProvinceInstance* const port = unit_manager.find_nearest_port(navy.get_country(), navy.get_location(), map_instance);
		if (port == nullptr) {
			unit_manager.destroy_group(navy);
			return;
		}
		std::vector<ArmyInstance*> carried;
		for (ArmyInstance& army : navy.get_carried_armies()) {
			carried.push_back(&army);
		}
		navy.set_location(*port);
		for (ArmyInstance* army : carried) {
			army->clear_path();
			army->set_location(*port);
		}
	}
}

void WarManager::update_naval_combat(InstanceManager& instance_manager) {
	MapInstance& map_instance = instance_manager.get_map_instance();
	UnitInstanceManager& unit_manager = instance_manager.get_unit_instance_manager();

	for (ProvinceInstance& province : map_instance.get_province_instances()) {
		if (province.get_navies().empty()) {
			continue;
		}

		std::vector<NavyInstance*> navies;
		for (NavyInstance& navy : province.get_navies()) {
			navy.set_in_combat(false);
			if (!navy.empty()) {
				navies.push_back(&navy);
			}
		}
		// Fleets in a port do not fight
		if (navies.size() < 2 || !province.province_definition.is_water()) {
			continue;
		}

		CountryInstance const* lead_country = nullptr;
		for (size_t i = 0; i < navies.size() && lead_country == nullptr; ++i) {
			for (size_t j = i + 1; j < navies.size(); ++j) {
				if (navies[i]->get_country().is_at_war_with(navies[j]->get_country())) {
					lead_country = &navies[i]->get_country();
					break;
				}
			}
		}
		if (lead_country == nullptr) {
			continue;
		}

		NavalSide side_a, side_b;
		for (NavyInstance* navy : navies) {
			CountryInstance const& country = navy->get_country();
			if (country.is_at_war_with(*lead_country)) {
				side_b.navies.push_back(navy);
			} else if (&country == lead_country) {
				side_a.navies.push_back(navy);
			} else if (std::any_of(navies.begin(), navies.end(), [&country, lead_country](NavyInstance* other) -> bool {
				return other->get_country().is_at_war_with(*lead_country) && country.is_at_war_with(other->get_country());
			})) {
				// An ally of the lead country fights alongside it
				side_a.navies.push_back(navy);
			}
		}

		calculate_naval_side(side_a);
		calculate_naval_side(side_b);
		if (side_a.ship_count == 0 || side_b.ship_count == 0) {
			continue;
		}
		const fixed_point_t power_a = side_a.power;
		const fixed_point_t power_b = side_b.power;
		apply_naval_damage(side_b, power_a);
		apply_naval_damage(side_a, power_b);
		calculate_naval_side(side_a);
		calculate_naval_side(side_b);

		const fixed_point_t rout_threshold = fixed_point_t(1) / 10;
		const bool routed_a = side_a.ship_count == 0 || side_a.get_organisation_ratio() < rout_threshold;
		const bool routed_b = side_b.ship_count == 0 || side_b.get_organisation_ratio() < rout_threshold;

		if (!routed_a && !routed_b) {
			for (NavyInstance* navy : side_a.navies) {
				navy->set_in_combat(true);
			}
			for (NavyInstance* navy : side_b.navies) {
				navy->set_in_combat(true);
			}
			continue;
		}

		const bool a_loses = routed_a && (!routed_b || side_a.get_organisation_ratio() <= side_b.get_organisation_ratio());
		NavalSide& loser = a_loses ? side_a : side_b;
		NavalSide& winner = a_loses ? side_b : side_a;

		// Sinking ships make the battle score of the winner
		if (!loser.navies.empty() && !winner.navies.empty()) {
			CountryInstance const& winner_country = winner.navies.front()->get_country();
			CountryInstance const& loser_country = loser.navies.front()->get_country();
			if (War* const war = find_war_between(winner_country, loser_country); war != nullptr) {
				const fixed_point_t gain = fixed_point_t::_1 + fixed_point_t(static_cast<int32_t>(std::min<size_t>(loser.navies.size() * 2, 12))) / 4;
				fixed_point_t& score = war->is_attacker(winner_country) ? war->attacker_battle_score : war->defender_battle_score;
				score += gain;
				if (score > MAX_BATTLE_SCORE) {
					score = fixed_point_t(MAX_BATTLE_SCORE);
				}
			}
		}

		for (NavyInstance* navy : loser.navies) {
			retreat_navy(*navy, unit_manager, map_instance);
		}
	}
}

void WarManager::update_blockades(InstanceManager& instance_manager) {
	MapInstance& map_instance = instance_manager.get_map_instance();
	blockaded_ports.clear();
	blockade_shares.clear();

	std::unordered_map<CountryInstance const*, std::pair<int32_t, int32_t>> counts; // ports, blockaded

	for (ProvinceInstance const& port : map_instance.get_province_instances()) {
		if (port.province_definition.is_water() || !port.province_definition.has_port()) {
			continue;
		}
		CountryInstance const* const owner = port.get_owner();
		ProvinceDefinition const* const sea_definition = port.province_definition.get_port_adjacent_province();
		if (owner == nullptr || sea_definition == nullptr) {
			continue;
		}
		counts[owner].first++;

		ProvinceInstance const& sea = map_instance.get_province_instance_by_definition(*sea_definition);
		bool enemy_fleet = false;
		bool defending_fleet = false;
		for (NavyInstance const& navy : sea.get_navies()) {
			if (navy.empty()) {
				continue;
			}
			if (navy.get_country().is_at_war_with(*owner)) {
				enemy_fleet = true;
			} else if (&navy.get_country() == owner || navy.get_country().has_alliance_with(*owner)) {
				defending_fleet = true;
			}
		}
		if (enemy_fleet && !defending_fleet) {
			blockaded_ports.insert(&port);
			counts[owner].second++;
		}
	}

	for (auto const& [country, count] : counts) {
		if (count.second > 0 && count.first > 0) {
			blockade_shares[country] = fixed_point_t(count.second) / count.first;
		}
	}
}

void WarManager::update_attrition(InstanceManager& instance_manager) {
	ModifierEffectCache const& cache = instance_manager.definition_manager.get_modifier_manager().get_modifier_effect_cache();

	// Share of the maximum strength lost each day outside of the own lands, and extra for armies without supplies
	const fixed_point_t base_attrition = fixed_point_t(2) / 1000;
	const fixed_point_t supply_attrition = fixed_point_t(3) / 1000;
	// Attrition never kills a regiment: it stops at this share of the strength
	const fixed_point_t floor_share = fixed_point_t(1) / 5;

	for (ProvinceInstance& province : instance_manager.get_map_instance().get_province_instances()) {
		if (province.get_armies().empty() || province.province_definition.is_water()) {
			continue;
		}
		CountryInstance const* const owner = province.get_owner();

		for (ArmyInstance& army : province.get_armies()) {
			CountryInstance& country = army.get_country();
			if (country.is_rebel_country()) {
				continue;
			}

			const bool friendly = owner == &country || (owner != nullptr && country.has_alliance_with(*owner));
			// Only countries at war need supplies, a blockade of the ports cuts them
			fixed_point_t supply = country.is_at_war() ? country.get_army_supply_ratio() : fixed_point_t::_1;
			supply -= get_blockade_share(country) / 2;
			if (supply < fixed_point_t::_0) {
				supply = fixed_point_t::_0;
			}

			fixed_point_t rate = fixed_point_t::_0;
			if (!friendly) {
				rate += base_attrition;
			}
			if (supply < fixed_point_t::_1) {
				rate += supply_attrition * (fixed_point_t::_1 - supply) * (friendly ? fixed_point_t::_0_50 : fixed_point_t::_1);
			}
			if (rate <= fixed_point_t::_0) {
				continue;
			}

			// Modifiers of the country (technology, ...) and the general change the attrition
			fixed_point_t multiplier = fixed_point_t::_1;
			if (cache.get_land_attrition() != nullptr) {
				multiplier += country.get_modifier_effect_value(*cache.get_land_attrition());
			}
			multiplier += get_leader_bonus(army, cache).attrition;
			if (multiplier <= fixed_point_t::_0) {
				continue;
			}
			rate *= multiplier;

			for (RegimentInstance& regiment : army.get_regiment_instances()) {
				const fixed_point_t floor_strength = regiment.get_max_strength() * floor_share;
				if (regiment.get_strength() <= floor_strength) {
					continue;
				}
				const fixed_point_t loss = std::min(regiment.get_max_strength() * rate, regiment.get_strength() - floor_strength);
				regiment.take_damage(fixed_point_t::_0, loss);
			}
		}
	}
}

void WarManager::update_leaders(InstanceManager& instance_manager) {
	UnitInstanceManager& unit_manager = instance_manager.get_unit_instance_manager();
	const Date today = instance_manager.get_today();

	// Free leaders (the best prestige first) go to groups that have none
	const auto assign_free = [](auto&& groups, auto&& leaders) -> void {
		for (auto const& wrapped : groups) {
			auto& group = wrapped.get();
			if (group.get_leader() != nullptr || group.empty()) {
				continue;
			}
			LeaderInstance* best = nullptr;
			for (LeaderInstance& leader : leaders) {
				if (leader.get_unit_instance_group() == nullptr && leader.get_can_be_used()
					&& (best == nullptr || leader.get_prestige() > best->get_prestige())) {
					best = &leader;
				}
			}
			if (best == nullptr) {
				break;
			}
			group.set_leader(best);
		}
	};

	for (CountryInstance& country : instance_manager.get_country_instance_manager().get_country_instances()) {
		if (country.is_rebel_country() || !country.exists()) {
			continue;
		}

		const size_t army_count = country.get_armies().size();
		const size_t navy_count = country.get_navies().size();
		if (army_count == 0 && navy_count == 0) {
			continue;
		}

		// Once a month new leaders are made as long as there are leadership points and groups without a leader;
		// generals first, then admirals
		if (today.is_month_start() && country.get_auto_create_leaders() && country.get_create_leader_count() > 0) {
			if (country.get_leaders<unit_branch_t::LAND>().size() < army_count) {
				unit_manager.create_leader(country, unit_branch_t::LAND, today);
			} else if (country.get_leaders<unit_branch_t::NAVAL>().size() < navy_count) {
				unit_manager.create_leader(country, unit_branch_t::NAVAL, today);
			}
		}

		if (!country.get_auto_assign_leaders()) {
			continue;
		}
		assign_free(country.get_armies(), country.get_leaders<unit_branch_t::LAND>());
		assign_free(country.get_navies(), country.get_leaders<unit_branch_t::NAVAL>());
	}
}

void WarManager::tick(InstanceManager& instance_manager) {
	fought_today.clear();

	update_combat(instance_manager);

	// Delete regiments that lost all their men
	{
		UnitInstanceManager& unit_manager = instance_manager.get_unit_instance_manager();
		for (ProvinceInstance& province : instance_manager.get_map_instance().get_province_instances()) {
			if (province.get_armies().empty()) {
				continue;
			}
			for (ArmyInstance* army : copy_armies(province)) {
				std::vector<UnitInstance*> dead;
				for (UnitInstance& unit : army->get_units()) {
					if (unit.get_strength() <= fixed_point_t::_0) {
						dead.push_back(&unit);
					}
				}
				// destroy_unit deletes the group together with its last unit
				for (UnitInstance* unit : dead) {
					unit_manager.destroy_unit(*unit, *army);
				}
			}
		}
	}

	update_naval_combat(instance_manager);
	// Sink the ships that lost all their men, the armies they cannot carry any more drown
	{
		UnitInstanceManager& unit_manager = instance_manager.get_unit_instance_manager();
		std::vector<unique_id_t> damaged;
		for (ProvinceInstance& province : instance_manager.get_map_instance().get_province_instances()) {
			for (NavyInstance& navy : province.get_navies()) {
				for (UnitInstance const& unit : navy.get_units()) {
					if (unit.get_strength() <= fixed_point_t::_0) {
						damaged.push_back(navy.unique_id);
						break;
					}
				}
			}
		}
		for (const unique_id_t navy_id : damaged) {
			UnitInstanceGroup* const group = unit_manager.get_unit_instance_group_by_unique_id(navy_id);
			if (group == nullptr) {
				continue;
			}
			std::vector<UnitInstance*> dead;
			for (UnitInstance& unit : group->get_units()) {
				if (unit.get_strength() <= fixed_point_t::_0) {
					dead.push_back(&unit);
				}
			}
			for (UnitInstance* unit : dead) {
				unit_manager.destroy_unit(*unit, *group);
			}
			if (UnitInstanceGroup* const remaining = unit_manager.get_unit_instance_group_by_unique_id(navy_id)) {
				unit_manager.enforce_transport_capacity(static_cast<NavyInstance&>(*remaining));
			}
		}
	}
	update_blockades(instance_manager);

	update_sieges(instance_manager);
	update_scores(instance_manager);
	update_army_recovery(instance_manager);
	update_attrition(instance_manager);
	update_leaders(instance_manager);
	update_ai_peace(instance_manager);
	update_auto_plan(instance_manager);
	update_fronts(instance_manager);
}

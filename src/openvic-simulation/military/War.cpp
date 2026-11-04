#include "War.hpp"

#include "openvic-simulation/military/SupplyTerritory.hpp"

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
#include "openvic-simulation/map/MapDefinition.hpp"
#include "openvic-simulation/map/TerrainType.hpp"
#include "openvic-simulation/military/Division.hpp"
#include "openvic-simulation/military/LeaderTrait.hpp"
#include "openvic-simulation/military/UnitModifiers.hpp"
#include "openvic-simulation/military/UnitType.hpp"
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

std::int32_t WarManager::get_province_weight(ProvinceInstance const& province) const {
	CountryInstance const* const owner = province.get_owner();
	if (owner != nullptr && owner->get_capital() == &province) {
		return get_war_defines().capital_province_weight;
	}
	return 1;
}

std::int32_t WarManager::get_siege_days_required(ProvinceInstance const& province) const {
	std::int32_t fort_levels = 0;
	for (BuildingInstance const& building : province.get_buildings()) {
		fort_levels += static_cast<std::int32_t>(type_safe::get(building.get_level()))
			* static_cast<std::int32_t>(type_safe::get(building.building_type.fort_level));
	}
	return get_war_defines().siege_base_days + fort_levels * get_war_defines().siege_days_per_fort_level;
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
	OpenVicPeaceDefines const& pd = get_peace_defines();

	switch (peace_type) {
	case peace_type_t::WHITE_PEACE:
		// The side that is clearly winning refuses, unless the war has dragged on for years
		return other_score - proposer_score < pd.white_peace_score_gap || days > pd.white_peace_war_days;
	case peace_type_t::TAKE_STATES: {
		if (proposer_score >= pd.surrender_score) {
			return true;
		}
		// Surrender when the capital of the leader has fallen
		memory::vector<CountryInstance*> const& other_side = war.get_side(!proposer_is_attacker);
		memory::vector<CountryInstance*> const& proposer_side = war.get_side(proposer_is_attacker);
		if (!other_side.empty()) {
			CountryInstance const* const leader = other_side.front();
			ProvinceInstance const* const capital = leader->get_capital();
			if (capital != nullptr && proposer_score >= pd.capital_lost_score && contains_country(proposer_side, capital->get_controller())) {
				return true;
			}
		}
		return days > pd.long_war_days && proposer_score >= pd.long_war_score;
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

			candidates.push_back({ state, loser, recipient, fixed_point_t(state_weight) / total_weight * get_war_defines().war_score_max });
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
			truces.push_back({ attacker, defender, today + Timespan::from_days(get_war_defines().truce_days) });
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

namespace {
	// A division in a land battle (docs/WAR_ASSUMPTIONS.md, section 6): what it is worth today and where it is
	struct BattleUnit {
		ArmyInstance* army = nullptr;
		std::uint32_t division_id = 0;
		DivisionStats const* stats = nullptr;
		bool front = false;
		bool retreats = false;
		fixed_point_t attack; // with the buffs of its types, its general and its supply
		fixed_point_t breakthrough;
		fixed_point_t protection; // a defender: its defence with the fortification and the terrain; an attacker: its breakthrough
		fixed_point_t conditions = fixed_point_t::_1; // an attacker: the terrain, the river, the night, the weather
		ProvinceInstance const* origin = nullptr; // an attacker: the province it came (or comes) from, for the river
		battle_formulas::Hit hit;
	};

	struct BattleSideUnits {
		std::vector<ArmyInstance*> armies;
		std::vector<BattleUnit> units;
	};

	// The buff of a regiment type in the battle: its general buff, the one of the terrain of the province and the ones against the types of the
	// enemy front (in proportion to the share of the enemy regiments they are against)
	RegimentType::OvBuff type_buff(
		RegimentType const& regiment_type, std::string_view terrain, std::vector<std::pair<RegimentType const*, std::uint32_t>> const& enemy_types,
		const std::uint32_t enemy_regiments
	) {
		RegimentType::OvBuff buff = regiment_type.ov.general;
		const auto add = [&buff](RegimentType::OvBuff const& other, const fixed_point_t share) {
			buff.attack_mult += other.attack_mult * share;
			buff.defense_mult += other.defense_mult * share;
			buff.breakthrough_mult += other.breakthrough_mult * share;
		};
		for (auto const& [identifier, terrain_buff] : regiment_type.ov.terrain) {
			if (identifier == terrain) {
				add(terrain_buff, fixed_point_t::_1);
			}
		}
		if (enemy_regiments > 0) {
			for (auto const& [identifier, vs_buff] : regiment_type.ov.vs_unit) {
				std::uint32_t matching = 0;
				for (auto const& [enemy_type, count] : enemy_types) {
					if (enemy_type->get_identifier() == identifier || get_unit_category_scope(*enemy_type) == identifier) {
						matching += count;
					}
				}
				if (matching > 0) {
					add(vs_buff, fixed_point_t(static_cast<std::int32_t>(matching)) / static_cast<std::int32_t>(enemy_regiments));
				}
			}
		}
		return buff;
	}

	// One day of a battle worked out without changing the game (docs/WAR_ASSUMPTIONS.md, section 6): the width, the divisions of the front (the ones
	// of front_divisions keep their places, the reserve takes the free ranks; one division always fights), what every division of the front is worth
	// today (the parts of its types with their buffs, its general, its supply; the protection of a defender with its fortification, its dig in and
	// the terrain, of an attacker its breakthrough; the conditions of an attacker: the terrain, the river from its origin, the night, the weather), the
	// hit every division of the front takes (battle_hit, the attack of the enemy side shared by the width) and R. The battle and the estimate use it.
	struct BattleDay {
		std::uint32_t width = 0, attacker_ranks = 0, defender_ranks = 0;
		fixed_point_t attacker_attack, defender_attack, attacker_conditions, ratio;
	};

	BattleDay work_out_battle_day(
		InstanceManager const& instance_manager, ProvinceInstance const& province, BattleSideUnits& attackers, BattleSideUnits& defenders,
		std::span<const std::uint32_t> front_divisions
	) {
		ModifierEffectCache const& cache = instance_manager.definition_manager.get_modifier_manager().get_modifier_effect_cache();
		BattleConditions const& conditions = instance_manager.definition_manager.get_military_manager().get_battle_conditions();
		MapDefinition const& map_definition = instance_manager.definition_manager.get_map_definition();
		OpenVicWarDefines const& wd = instance_manager.definition_manager.get_define_manager().get_openvic_defines().war;
	// The width of the battle: the divisions that were in the front stay, the reserve takes the free ranks (one division always fights)
	TerrainType const* const terrain = province.get_terrain_type();
	const fixed_point_t width_share = fixed_point_t::_1 + (terrain != nullptr ? terrain->get_combat_width_percentage_change() : fixed_point_t::_0);
	const std::uint32_t width = static_cast<std::uint32_t>(std::max(1, (width_share * wd.battle_width_base).floor<std::int32_t>()));
	const auto choose_front = [&front_divisions, width](BattleSideUnits& side) -> std::uint32_t {
		std::uint32_t used = 0;
		for (const bool kept : { true, false }) {
			for (BattleUnit& unit : side.units) {
				const bool was_front = std::find(front_divisions.begin(), front_divisions.end(), unit.division_id) != front_divisions.end();
				if (unit.front || was_front != kept) {
					continue;
				}
				if (used == 0 || used + unit.stats->width <= width) {
					unit.front = true;
					used += unit.stats->width;
				}
			}
		}
		return used;
	};
	const std::uint32_t attacker_ranks = choose_front(attackers);
	const std::uint32_t defender_ranks = choose_front(defenders);

	// What the regiment types of a front are, for the buffs against them
	const auto front_types = [](BattleSideUnits const& side, std::uint32_t& regiments) {
		std::vector<std::pair<RegimentType const*, std::uint32_t>> types;
		regiments = 0;
		for (BattleUnit const& unit : side.units) {
			if (!unit.front) {
				continue;
			}
			for (DivisionStats::TypePart const& part : unit.stats->parts) {
				auto it = std::find_if(types.begin(), types.end(), [&part](auto const& entry) -> bool { return entry.first == part.type; });
				if (it == types.end()) {
					types.emplace_back(part.type, part.regiments);
				} else {
					it->second += part.regiments;
				}
				regiments += part.regiments;
			}
		}
		return types;
	};
	std::uint32_t attacker_regiments = 0, defender_regiments = 0;
	const auto attacker_types = front_types(attackers, attacker_regiments);
	const auto defender_types = front_types(defenders, defender_regiments);
	const std::string_view terrain_id = terrain != nullptr ? std::string_view { terrain->get_identifier() } : std::string_view {};
	const fixed_point_t terrain_defence = fixed_point_t::_1 + wd.terrain_defence_per_point.of(terrain != nullptr ? terrain->get_defence_bonus() : fixed_point_t::_0);
	const fixed_point_t attacker_terrain = conditions.get_terrain(terrain_id) * conditions.night * conditions.weather;

	// What every division of the front is worth today: the parts of its types with their buffs, its general, its supply, the bonus of the planning of its
	// field marshal (P6)
	const auto evaluate = [&](BattleSideUnits& side, const bool attacking, std::vector<std::pair<RegimentType const*, std::uint32_t>> const& enemy_types,
		const std::uint32_t enemy_regiments) {
		for (BattleUnit& unit : side.units) {
			if (!unit.front) {
				continue;
			}
			const LeaderBonus bonus = get_leader_bonus(*unit.army, cache);
			const fixed_point_t leader_attack = std::max(wd.leader_min_multiplier.fixed(), fixed_point_t::_1 + bonus.attack / wd.leader_point_divisor);
			const fixed_point_t leader_defence = std::max(wd.leader_min_multiplier.fixed(), fixed_point_t::_1 + bonus.defence / wd.leader_point_divisor);
			const fixed_point_t supply = unit.army->get_supply_level();
			fixed_point_t attack = fixed_point_t::_0, defence = fixed_point_t::_0, breakthrough = fixed_point_t::_0;
			for (DivisionStats::TypePart const& part : unit.stats->parts) {
				const RegimentType::OvBuff buff = type_buff(*part.type, terrain_id, enemy_types, enemy_regiments);
				attack += part.attack * (fixed_point_t::_1 + buff.attack_mult);
				defence += part.defence * (fixed_point_t::_1 + buff.defense_mult);
				breakthrough += part.breakthrough * (fixed_point_t::_1 + buff.breakthrough_mult);
			}
			const fixed_point_t planning = fixed_point_t::_1 + get_army_planning(*unit.army);
			unit.attack = attack * leader_attack * supply * planning;
			unit.breakthrough = breakthrough * leader_attack * supply * planning;
			if (attacking) {
				unit.protection = unit.breakthrough;
				// Over a river: the army came from a province whose border with this one runs along a river
				ProvinceInstance const* const from = unit.origin;
				const bool river = from != nullptr
					&& map_definition.get_river_border_share(from->province_definition, province.province_definition) >= wd.river_border_min_share.fixed();
				unit.conditions = attacker_terrain * (river ? conditions.river_crossing : fixed_point_t::_1);
			} else {
				const fixed_point_t fortification = unit.stats->fortification + fixed_point_t(static_cast<std::int32_t>(unit.army->get_dig_in_level()));
				unit.protection = battle_formulas::defender_protection(wd, defence * leader_defence * supply, fortification, terrain_defence);
			}
		}
	};
	evaluate(attackers, true, defender_types, defender_regiments);
	evaluate(defenders, false, attacker_types, attacker_regiments);

	// The attack of a side is shared by the divisions of the enemy front in proportion to their width
	const auto side_attack = [](BattleSideUnits const& side, fixed_point_t& with_conditions) -> fixed_point_t {
		fixed_point_t attack = fixed_point_t::_0;
		with_conditions = fixed_point_t::_0;
		for (BattleUnit const& unit : side.units) {
			if (unit.front) {
				attack += unit.attack;
				with_conditions += unit.attack * unit.conditions;
			}
		}
		return attack;
	};
	fixed_point_t attacker_conditioned = fixed_point_t::_0, defender_conditioned = fixed_point_t::_0;
	const fixed_point_t attacker_attack = side_attack(attackers, attacker_conditioned);
	const fixed_point_t defender_attack = side_attack(defenders, defender_conditioned);
	const fixed_point_t attacker_conditions = attacker_attack > fixed_point_t::_0 ? attacker_conditioned / attacker_attack : fixed_point_t::_1;
	const auto strike = [&wd](BattleSideUnits& targets, const std::uint32_t target_ranks, const fixed_point_t attack, const fixed_point_t side_conditions) {
		for (BattleUnit& unit : targets.units) {
			if (unit.front && target_ranks > 0) {
				const fixed_point_t directed = attack * static_cast<std::int32_t>(unit.stats->width) / static_cast<std::int32_t>(target_ranks);
				unit.hit = battle_formulas::battle_hit(wd, directed, side_conditions, unit.stats->density, unit.protection, unit.stats->casualties_taken);
			}
		}
	};
	strike(defenders, defender_ranks, attacker_attack, attacker_conditions);
	strike(attackers, attacker_ranks, defender_attack, fixed_point_t::_1);

	// R of the day: the breakthrough of the attacking front over the protection of the defending one (before the losses)
	fixed_point_t breakthrough_sum = fixed_point_t::_0, protection_sum = fixed_point_t::_0;
	for (BattleUnit const& unit : attackers.units) {
		breakthrough_sum += unit.front ? unit.breakthrough : fixed_point_t::_0;
	}
	for (BattleUnit const& unit : defenders.units) {
		protection_sum += unit.front ? unit.protection : fixed_point_t::_0;
	}
	const fixed_point_t ratio = battle_formulas::breakthrough_ratio(breakthrough_sum, protection_sum);
		BattleDay day;
		day.width = width;
		day.attacker_ranks = attacker_ranks;
		day.defender_ranks = defender_ranks;
		day.attacker_attack = attacker_attack;
		day.defender_attack = defender_attack;
		day.attacker_conditions = attacker_conditions;
		day.ratio = battle_formulas::breakthrough_ratio(breakthrough_sum, protection_sum);
		return day;
	}

	// The organisation of the division (the average of its regiments, points)
	fixed_point_t division_organisation(ArmyInstance const& army, const std::uint32_t division_id) {
		fixed_point_t sum = fixed_point_t::_0;
		std::int32_t count = 0;
		for (RegimentInstance const& regiment : army.get_regiment_instances()) {
			if (regiment.get_division_id() == division_id) {
				sum += regiment.get_organisation();
				count++;
			}
		}
		return count > 0 ? sum / count : fixed_point_t::_0;
	}

	// The men a division loses come from its first line (docs/WAR_ASSUMPTIONS.md, section 6): the first regiment of every rank that has men takes an
	// equal share; what a regiment cannot take goes to the next line of its rank. The organisation: every regiment loses the points in proportion to its
	// maximum (the average of the division falls by the points)
	fixed_point_t apply_division_losses(
		UnitTypeManager const& unit_type_manager, CountryInstance const& country, ArmyInstance& army, DivisionInstance const& division,
		const std::uint32_t extra_depth, battle_formulas::Hit const& hit
	) {
		const DivisionLayout layout = layout_division(unit_type_manager, country, army, division, extra_depth);
		std::span<const std::reference_wrapper<RegimentInstance>> const regiments = army.get_regiment_instances();
		// The ranks with their regiments, the first line first
		std::vector<std::vector<RegimentInstance*>> ranks;
		fixed_point_t max_organisation_sum = fixed_point_t::_0;
		std::int32_t regiment_count = 0;
		for (DivisionLayout::Entry const& entry : layout.entries) {
			if (ranks.empty() || entry.line == 0) {
				ranks.emplace_back();
			}
			RegimentInstance& regiment = regiments[entry.regiment];
			ranks.back().push_back(&regiment);
			max_organisation_sum += regiment.get_max_organisation();
			regiment_count++;
		}
		fixed_point_t lost = fixed_point_t::_0;
		if (ranks.empty()) {
			return lost;
		}
		const fixed_point_t share = hit.strength_loss / static_cast<std::int32_t>(ranks.size());
		for (std::vector<RegimentInstance*> const& rank : ranks) {
			fixed_point_t left = share;
			for (RegimentInstance* regiment : rank) {
				if (left <= fixed_point_t::_0) {
					break;
				}
				const fixed_point_t taken = std::min(left, regiment->get_strength());
				regiment->take_damage(fixed_point_t::_0, taken);
				lost += taken;
				left -= taken;
			}
		}
		const fixed_point_t mean_max_organisation = max_organisation_sum / regiment_count;
		for (std::vector<RegimentInstance*> const& rank : ranks) {
			for (RegimentInstance* regiment : rank) {
				const fixed_point_t loss = mean_max_organisation > fixed_point_t::_0
					? hit.organisation_loss * regiment->get_max_organisation() / mean_max_organisation : hit.organisation_loss;
				regiment->take_damage(loss, fixed_point_t::_0);
			}
		}
		return lost;
	}
}

DivisionStats const& WarManager::get_live_division_stats(
	InstanceManager const& instance_manager, ArmyInstance const& army, DivisionInstance const& division
) const {
	CountryInstance const& country = army.get_country();
	// The signature of what the numbers depend on: the day (the modifiers and the technologies of the country are summed every day), the country, the
	// division and its template, the general, every regiment with its men and its weapons
	std::uint64_t signature = 1469598103934665603ULL;
	const auto mix = [&signature](const std::uint64_t value) {
		signature = (signature ^ value) * 1099511628211ULL;
	};
	mix(static_cast<std::uint64_t>((instance_manager.get_today() - Date {}).to_int()));
	mix(static_cast<std::uint64_t>(type_safe::get(country.index)));
	mix(division.id);
	mix(division.template_id);
	mix(army.get_leader() != nullptr ? army.get_leader()->unique_id : 0);
	if (DivisionTemplate const* const division_template = division.template_id != 0 ? country.find_division_template(division.template_id) : nullptr) {
		for (const std::uint8_t cell : division_template->cells) {
			mix(cell);
		}
		for (const std::uint8_t company : division_template->companies) {
			mix(company);
		}
	}
	for (RegimentInstance const& regiment : army.get_regiment_instances()) {
		if (regiment.get_division_id() != division.id) {
			continue;
		}
		mix(regiment.unique_id);
		mix(static_cast<std::uint64_t>(regiment.get_strength().get_raw_value()));
		mix(static_cast<std::uint64_t>(regiment.get_weapon_attack_ratio().get_raw_value()));
		mix(static_cast<std::uint64_t>(regiment.get_weapon_defence_ratio().get_raw_value()));
		mix(static_cast<std::uint64_t>(regiment.get_weapon_breakthrough_ratio().get_raw_value()));
	}
	LiveStatsEntry& entry = live_stats_cache[division.id];
	if (entry.signature != signature || entry.stats.regiments == 0) {
		entry.signature = signature;
		entry.stats = compute_live_division_stats(instance_manager, country, army, division);
		live_stats_computed++;
	}
	return entry.stats;
}

BattleEstimate WarManager::estimate_battle(
	InstanceManager const& instance_manager, std::span<ArmyInstance const* const> attacking_armies, ProvinceInstance const& target
) const {
	BattleEstimate estimate;
	if (attacking_armies.empty()) {
		return estimate;
	}
	CountryInstance const& country = attacking_armies.front()->get_country();
	// The work out of a day only reads the armies (the pointers are not const because the battle changes them afterwards)
	BattleSideUnits attackers, defenders;
	for (ArmyInstance const* army : attacking_armies) {
		attackers.armies.push_back(const_cast<ArmyInstance*>(army));
	}
	for (ArmyInstance const& army : target.get_armies()) {
		if (army.get_transport_navy() == nullptr && !army.empty() && army.get_country().is_at_war_with(country)) {
			defenders.armies.push_back(const_cast<ArmyInstance*>(&army));
		}
	}
	const auto gather = [&](BattleSideUnits& side, const bool attacking) {
		for (ArmyInstance* army : side.armies) {
			for (DivisionInstance const& division : army->get_divisions()) {
				DivisionStats const& stats = get_live_division_stats(instance_manager, *army, division);
				if (stats.regiments == 0) {
					continue;
				}
				BattleUnit unit;
				unit.army = army;
				unit.division_id = division.id;
				unit.stats = &stats;
				unit.origin = attacking ? (&army->get_location() != &target ? &army->get_location() : army->get_previous_location()) : nullptr;
				side.units.push_back(unit);
			}
		}
	};
	gather(attackers, true);
	gather(defenders, false);
	if (attackers.units.empty()) {
		return estimate;
	}
	estimate.valid = true;
	if (defenders.units.empty()) {
		// Nobody defends: the province is taken without a battle
		estimate.ratio = fixed_point_t(1000);
		estimate.breakthrough_days = 0;
		estimate.attacker_wins = true;
		estimate.win_chance = fixed_point_t::_1;
		return estimate;
	}
	OpenVicWarDefines const& wd = get_war_defines();
	LandBattle const* const battle = get_land_battle(target);
	const BattleDay day = work_out_battle_day(
		instance_manager, target, attackers, defenders, battle != nullptr ? std::span<const std::uint32_t> { battle->front_divisions } : std::span<const std::uint32_t> {}
	);
	estimate.ratio = day.ratio;
	estimate.attacker_attack = day.attacker_attack;
	estimate.defender_attack = day.defender_attack;
	// The days until the first division of the front of a side has no organisation left
	const auto side_days = [](BattleSideUnits const& side, fixed_point_t& damage) -> fixed_point_t {
		fixed_point_t days = fixed_point_t(1000);
		damage = fixed_point_t::_0;
		for (BattleUnit const& unit : side.units) {
			if (!unit.front) {
				continue;
			}
			damage += unit.hit.damage;
			if (unit.hit.organisation_loss > fixed_point_t::_0) {
				days = std::min(days, unit.stats->current_organisation / unit.hit.organisation_loss);
			}
		}
		return days;
	};
	estimate.attacker_days = side_days(attackers, estimate.attacker_damage);
	estimate.defender_days = side_days(defenders, estimate.defender_damage);
	estimate.breakthrough_days = day.ratio >= wd.break_ratio_day ? 1 : day.ratio >= wd.break_ratio_week ? static_cast<std::uint32_t>(wd.break_week_days) : 0;
	const fixed_point_t defence_ends = estimate.breakthrough_days > 0
		? std::min(estimate.defender_days, fixed_point_t(static_cast<std::int32_t>(estimate.breakthrough_days))) : estimate.defender_days;
	estimate.attacker_wins = defence_ends < estimate.attacker_days;
	estimate.win_chance = battle_formulas::win_chance(estimate.attacker_days, defence_ends, wd.battle_win_steepness);
	return estimate;
}

void WarManager::update_combat(InstanceManager& instance_manager) {
	MapInstance& map_instance = instance_manager.get_map_instance();
	UnitInstanceManager& unit_manager = instance_manager.get_unit_instance_manager();
	UnitTypeManager const& unit_type_manager = instance_manager.definition_manager.get_military_manager().get_unit_type_manager();
	OpenVicWarDefines const& wd = get_war_defines();
	battle_reports.clear();
	std::unordered_set<ProvinceInstance const*> active;

	// Live cache entries of divisions that no longer exist are dropped once a month
	if (instance_manager.get_today().is_month_start()) {
		std::unordered_set<std::uint32_t> alive;
		for (ProvinceInstance& province : map_instance.get_province_instances()) {
			for (ArmyInstance const& army : province.get_armies()) {
				for (DivisionInstance const& division : army.get_divisions()) {
					alive.insert(division.id);
				}
			}
		}
		std::erase_if(live_stats_cache, [&alive](auto const& entry) -> bool {
			return !alive.contains(entry.first);
		});
	}

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

		// A country that has an enemy in the province
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

		// The attacker of a battle that goes on stays the attacker; a new battle: the side of the controller of the province defends, else the side
		// that is dug in deeper (the other side came later), else the side of the first army of the province
		LandBattle& battle = land_battles[&province];
		const bool attacker_present = battle.attacker != nullptr && std::any_of(armies.begin(), armies.end(), [&battle](ArmyInstance* army) -> bool {
			return &army->get_country() == battle.attacker;
		});
		if (!attacker_present) {
			battle = {};
		}
		const CountryInstance* side_country = attacker_present ? battle.attacker : lead_country;
		const auto build_side = [&armies](CountryInstance const& leader, BattleSideUnits& side, BattleSideUnits& enemies) {
			for (ArmyInstance* army : armies) {
				if (!army->empty() && army->get_country().is_at_war_with(leader)) {
					enemies.armies.push_back(army);
				}
			}
			for (ArmyInstance* army : armies) {
				if (army->empty()) {
					continue;
				}
				CountryInstance const& country = army->get_country();
				if (&country == &leader) {
					side.armies.push_back(army);
					continue;
				}
				if (country.is_at_war_with(leader)) {
					continue;
				}
				// An ally fights alongside if it is at war with someone on the other side
				if (std::any_of(enemies.armies.begin(), enemies.armies.end(), [&country](ArmyInstance* enemy) -> bool {
					return country.is_at_war_with(enemy->get_country());
				})) {
					side.armies.push_back(army);
				}
			}
		};
		BattleSideUnits attackers, defenders;
		build_side(*side_country, attackers, defenders);
		if (attackers.armies.empty() || defenders.armies.empty()) {
			continue;
		}
		if (!attacker_present) {
			CountryInstance const* const controller = province.get_controller();
			const auto has_country = [](BattleSideUnits const& side, CountryInstance const* country) -> bool {
				return country != nullptr && std::any_of(side.armies.begin(), side.armies.end(), [country](ArmyInstance* army) -> bool {
					return &army->get_country() == country || (!army->get_country().is_at_war_with(*country) && country->has_alliance_with(army->get_country()));
				});
			};
			const auto dig_in = [](BattleSideUnits const& side) -> std::int32_t {
				std::int32_t best = 0;
				for (ArmyInstance* army : side.armies) {
					best = std::max<std::int32_t>(best, army->get_dig_in_level());
				}
				return best;
			};
			bool swap = false;
			if (has_country(attackers, controller) && !has_country(defenders, controller)) {
				swap = true;
			} else if (!has_country(defenders, controller) && dig_in(attackers) > dig_in(defenders)) {
				swap = true;
			}
			if (swap) {
				std::swap(attackers, defenders);
			}
			battle.attacker = &attackers.armies.front()->get_country();
		}
		active.insert(&province);
		battle.days++;

		// The divisions of both sides, in the order of the armies of the province
		const auto gather = [&](BattleSideUnits& side) {
			for (ArmyInstance* army : side.armies) {
				for (DivisionInstance const& division : army->get_divisions()) {
					DivisionStats const& stats = get_live_division_stats(instance_manager, *army, division);
					if (stats.regiments == 0) {
						continue;
					}
					BattleUnit unit;
					unit.army = army;
					unit.division_id = division.id;
					unit.stats = &stats;
					unit.origin = army->get_previous_location();
					side.units.push_back(unit);
				}
			}
		};
		gather(attackers);
		gather(defenders);
		if (attackers.units.empty() || defenders.units.empty()) {
			continue;
		}

		const BattleDay battle_day = work_out_battle_day(instance_manager, province, attackers, defenders, battle.front_divisions);
		const std::uint32_t width = battle_day.width;
		const std::uint32_t attacker_ranks = battle_day.attacker_ranks;
		const std::uint32_t defender_ranks = battle_day.defender_ranks;
		const fixed_point_t attacker_attack = battle_day.attacker_attack;
		const fixed_point_t defender_attack = battle_day.defender_attack;
		const fixed_point_t attacker_conditions = battle_day.attacker_conditions;
		const fixed_point_t ratio = battle_day.ratio;
		const bool broken = battle_formulas::breakthrough_day(wd, ratio, battle.counter);

		// The losses
		LandBattleReport report;
		report.province = &province;
		report.attacker = battle.attacker;
		report.defender = &defenders.armies.front()->get_country();
		report.day = battle.days;
		report.width = width;
		report.attacker_ranks = attacker_ranks;
		report.defender_ranks = defender_ranks;
		report.attacker_attack = attacker_attack;
		report.defender_attack = defender_attack;
		report.conditions = attacker_conditions;
		report.ratio = ratio;
		report.breakthrough = broken;
		const std::uint32_t extra_depth = static_cast<std::uint32_t>(wd.template_depth_default);
		const auto take_losses = [&](BattleSideUnits& side, fixed_point_t& lost_total) {
			for (BattleUnit& unit : side.units) {
				if (!unit.front) {
					continue;
				}
				DivisionInstance const* division = nullptr;
				for (DivisionInstance const& candidate : unit.army->get_divisions()) {
					if (candidate.id == unit.division_id) {
						division = &candidate;
					}
				}
				if (division == nullptr) {
					continue;
				}
				const fixed_point_t lost = apply_division_losses(unit_type_manager, unit.army->get_country(), *unit.army, *division, extra_depth, unit.hit);
				lost_total += lost;
				battle_stats[&unit.army->get_country()].strength_lost += lost;
			}
		};
		take_losses(attackers, report.attacker_strength_lost);
		take_losses(defenders, report.defender_strength_lost);

		// Who leaves the battle: the whole defence when the breakthrough rule says so, every division with no organisation left
		const auto mark_retreats = [&](BattleSideUnits& side, const bool all) -> std::uint32_t {
			std::uint32_t count = 0;
			for (BattleUnit& unit : side.units) {
				unit.retreats = all || (unit.front && division_organisation(*unit.army, unit.division_id) <= fixed_point_t::_0);
				count += unit.retreats;
			}
			return count;
		};
		report.attacker_retreated = mark_retreats(attackers, false);
		report.defender_retreated = mark_retreats(defenders, broken);
		const auto summary = [&](BattleSideUnits const& side, fixed_point_t& strength, fixed_point_t& max_strength, fixed_point_t& organisation,
			std::uint32_t& front, std::uint32_t& reserve) {
			std::int32_t front_count = 0;
			for (BattleUnit const& unit : side.units) {
				for (RegimentInstance const& regiment : unit.army->get_regiment_instances()) {
					if (regiment.get_division_id() == unit.division_id) {
						strength += regiment.get_strength();
						max_strength += regiment.get_max_strength();
					}
				}
				if (unit.front) {
					organisation += division_organisation(*unit.army, unit.division_id);
					front_count++;
					front++;
				} else {
					reserve++;
				}
			}
			organisation = front_count > 0 ? organisation / front_count : fixed_point_t::_0;
		};
		summary(attackers, report.attacker_strength, report.attacker_max_strength, report.attacker_organisation, report.attacker_front, report.attacker_reserve);
		summary(defenders, report.defender_strength, report.defender_max_strength, report.defender_organisation, report.defender_front, report.defender_reserve);
		for (BattleSideUnits const* side : { &attackers, &defenders }) {
			std::vector<CountryInstance const*> counted;
			for (ArmyInstance* army : side->armies) {
				fought_today.insert(army->unique_id);
				if (std::find(counted.begin(), counted.end(), &army->get_country()) == counted.end()) {
					counted.push_back(&army->get_country());
					battle_stats[&army->get_country()].battle_days++;
				}
			}
		}

		const auto stays = [](BattleSideUnits const& side) -> bool {
			return std::any_of(side.units.begin(), side.units.end(), [](BattleUnit const& unit) -> bool { return !unit.retreats; });
		};
		const bool attackers_stay = stays(attackers);
		const bool defenders_stay = stays(defenders);
		report.ended = !attackers_stay || !defenders_stay;
		report.attacker_won = attackers_stay && !defenders_stay;

		// The front of tomorrow: the divisions of the front that stay
		battle.front_divisions.clear();
		for (BattleSideUnits const* side : { &attackers, &defenders }) {
			for (BattleUnit const& unit : side->units) {
				if (unit.front && !unit.retreats) {
					battle.front_divisions.push_back(unit.division_id);
				}
			}
		}

		// The end of the battle: the battle score of the winner, the prestige of the generals, a general of the losers may die
		if (report.ended && (attackers_stay || defenders_stay)) {
			BattleSideUnits const& winner = attackers_stay ? attackers : defenders;
			BattleSideUnits const& loser = attackers_stay ? defenders : attackers;
			std::size_t loser_regiments = 0;
			for (BattleUnit const& unit : loser.units) {
				loser_regiments += unit.stats->regiments;
			}
			for (BattleSideUnits const* side : { &loser, &winner }) {
				std::vector<CountryInstance const*> counted;
				for (ArmyInstance* army : side->armies) {
					if (std::find(counted.begin(), counted.end(), &army->get_country()) == counted.end()) {
						counted.push_back(&army->get_country());
						(side == &loser ? battle_stats[&army->get_country()].lost : battle_stats[&army->get_country()].won)++;
					}
				}
			}
			CountryInstance const& winner_country = winner.armies.front()->get_country();
			CountryInstance const& loser_country = loser.armies.front()->get_country();
			if (War* const war = find_war_between(winner_country, loser_country); war != nullptr) {
				const fixed_point_t gain = fixed_point_t::_1
					+ fixed_point_t(static_cast<int32_t>(std::min<size_t>(loser_regiments, static_cast<size_t>(wd.battle_score_regiment_cap))))
						/ wd.battle_score_regiment_divisor;
				fixed_point_t& score = war->is_attacker(winner_country) ? war->attacker_battle_score : war->defender_battle_score;
				score += gain;
				if (score > wd.max_battle_score) {
					score = fixed_point_t(wd.max_battle_score);
				}
			}
			for (ArmyInstance* army : winner.armies) {
				if (LeaderInstance* const leader = army->get_leader()) {
					leader->add_prestige(wd.leader_victory_prestige.fixed());
				}
			}
			for (ArmyInstance* army : loser.armies) {
				if (LeaderInstance* const leader = army->get_leader()) {
					leader->add_prestige(-wd.leader_defeat_prestige.fixed());
					// A deterministic "dice": about one lost battle in leader_death_odds kills the general
					const std::uint64_t dice = (leader->unique_id * 2654435761ULL + static_cast<std::uint64_t>((instance_manager.get_today() - Date {}).to_int()) * 40503ULL) >> 3;
					if (dice % static_cast<std::uint64_t>(wd.leader_death_odds) == 0) {
						unit_manager.destroy_leader(*leader);
					}
				}
			}
		}

		// The divisions that leave go to a neighbouring province (a division of an army that stays leaves as an army of its own); nowhere to go: destroyed
		for (BattleSideUnits* side : { &attackers, &defenders }) {
			for (ArmyInstance* army : side->armies) {
				std::vector<std::uint32_t> leaving;
				std::size_t divisions = 0;
				for (BattleUnit const& unit : side->units) {
					if (unit.army == army) {
						divisions++;
						if (unit.retreats) {
							leaving.push_back(unit.division_id);
						}
					}
				}
				if (leaving.empty()) {
					army->set_in_combat(!report.ended);
					continue;
				}
				std::vector<ArmyInstance*> movers;
				if (leaving.size() == divisions) {
					movers.push_back(army);
				} else {
					for (const std::uint32_t division_id : leaving) {
						if (ArmyInstance* const split = split_division(unit_manager, *army, division_id); split != nullptr) {
							movers.push_back(split);
						}
					}
					army->set_in_combat(!report.ended);
				}
				for (ArmyInstance* mover : movers) {
					mover->clear_path();
					mover->set_in_combat(false);
					fought_today.insert(mover->unique_id);
					ProvinceInstance* const retreat = find_retreat_province(province, *mover, map_instance);
					if (retreat != nullptr) {
						mover->set_location(*retreat);
					} else {
						unit_manager.destroy_group(*mover);
					}
				}
			}
		}
		battle_reports.push_back(report);
		if (report.ended) {
			active.erase(&province);
		}
	}

	std::erase_if(land_battles, [&active](auto const& entry) -> bool {
		return !active.contains(entry.first);
	});
}

void WarManager::update_sieges(InstanceManager& instance_manager) {
	OpenVicWarDefines const& wd = get_war_defines();
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
				siege_speed += wd.siege_speed_per_regiment.fixed() + regiment.get_regiment_type().siege / wd.siege_skill_divisor;
			}
		}
		siege_speed = std::min(siege_speed, fixed_point_t(wd.siege_speed_max));
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
	const std::int32_t score_max = get_war_defines().war_score_max;
	for (War& war : wars) {
		// Share of the enemies' land (by weight) that the side holds, 0..100
		const auto occupation_score = [this, score_max](
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
			return total > 0 ? fixed_point_t(held) / total * score_max : fixed_point_t::_0;
		};

		war.attacker_score = std::min<fixed_point_t>(
			occupation_score(war.defenders, war.attackers) + war.attacker_battle_score, fixed_point_t(score_max)
		);
		war.defender_score = std::min<fixed_point_t>(
			occupation_score(war.attackers, war.defenders) + war.defender_battle_score, fixed_point_t(score_max)
		);
	}
}

bool WarManager::is_supply_reached(CountryInstance const& country, ProvinceInstance const& province) const {
	const auto it = supply_reach.find(&country);
	if (it == supply_reach.end()) {
		return true;
	}
	const std::size_t index = type_safe::get(province.index);
	return index < it->second.size() && it->second[index] != 0;
}

void WarManager::update_encirclements(InstanceManager& instance_manager) {
	MapInstance& map_instance = instance_manager.get_map_instance();
	CountryInstanceManager const& countries = instance_manager.get_country_instance_manager();
	const std::size_t count = map_instance.get_province_instances().size();
	supply_reach.clear();
	std::deque<ProvinceInstance const*> queue;
	for (CountryInstance& country : instance_manager.get_country_instance_manager().get_country_instances()) {
		if (!country.exists() || !country.has_armies()) {
			continue;
		}
		if (!country.is_at_war()) {
			for (ArmyInstance& army : country.get_armies()) {
				army.set_encircled(false);
				army.set_encircled_days(0);
			}
			continue;
		}
		const SupplyFriends friends = SupplyFriends::build(country, countries, *this);
		std::vector<std::uint8_t>& reach = supply_reach[&country];
		reach.assign(count, 0);
		queue.clear();
		// The sources: the capital (held by a friend) and the ports with a naval base the country holds that no enemy blockades
		const auto add_source = [&](ProvinceInstance const& province) {
			const std::size_t index = type_safe::get(province.index);
			if (reach[index] == 0) {
				reach[index] = 1;
				queue.push_back(&province);
			}
		};
		if (country.get_capital() != nullptr && is_supply_held(*country.get_capital(), friends)) {
			add_source(*country.get_capital());
		}
		for (ProvinceInstance* province : country.get_owned_provinces()) {
			if (province->get_controller() != &country || !province->province_definition.has_port() || is_blockaded(*province)) {
				continue;
			}
			for (BuildingInstance const& building : province->get_buildings()) {
				if (building.building_type.get_identifier() == "naval_base" && type_safe::get(building.get_level()) > 0) {
					add_source(*province);
					break;
				}
			}
		}
		// A country without any source (its capital and its ports are lost) is not cut off as a whole: its armies keep what they have
		if (queue.empty()) {
			supply_reach.erase(&country);
			for (ArmyInstance& army : country.get_armies()) {
				army.set_encircled(false);
			}
			continue;
		}
		// Over land and over the straits (Constantinople and Anatolia, Denmark and Scania...)
		const auto connects = [](ProvinceDefinition::adjacency_t const& adjacency) -> bool {
			return !adjacency.get_to().is_water() && (adjacency.get_type() == ProvinceDefinition::adjacency_t::type_t::LAND
				|| adjacency.get_type() == ProvinceDefinition::adjacency_t::type_t::STRAIT);
		};
		while (!queue.empty()) {
			ProvinceInstance const* const current = queue.front();
			queue.pop_front();
			for (ProvinceDefinition::adjacency_t const& adjacency : current->province_definition.get_adjacencies()) {
				if (!connects(adjacency)) {
					continue;
				}
				ProvinceInstance const& next = map_instance.get_province_instance_by_definition(adjacency.get_to());
				const std::size_t index = type_safe::get(next.index);
				if (reach[index] == 0 && is_supply_held(next, friends)) {
					reach[index] = 1;
					queue.push_back(&next);
				}
			}
		}
		// An army is reached in its province, or next to it (it stands on the land of the enemy at the edge of ours: the last step leads to it)
		for (ArmyInstance& army : country.get_armies()) {
			ProvinceInstance const& at = army.get_location();
			bool reached = army.get_transport_navy() != nullptr || at.province_definition.is_water() || reach[type_safe::get(at.index)] != 0;
			if (!reached && !is_supply_held(at, friends)) {
				for (ProvinceDefinition::adjacency_t const& adjacency : at.province_definition.get_adjacencies()) {
					if (connects(adjacency) && reach[type_safe::get(map_instance.get_province_instance_by_definition(adjacency.get_to()).index)] != 0) {
						reached = true;
						break;
					}
				}
			}
			army.set_encircled(!reached);
			if (reached) {
				army.set_encircled_days(0);
			}
		}
	}
}

void WarManager::update_army_recovery(InstanceManager& instance_manager) {
	OpenVicWarDefines const& wd = get_war_defines();
	const fixed_point_t organisation_recovery = wd.organisation_recovery.fixed();
	const fixed_point_t strength_recovery = wd.strength_recovery.fixed();
	std::vector<ArmyInstance*> surrendered;

	for (ProvinceInstance& province : instance_manager.get_map_instance().get_province_instances()) {
		for (ArmyInstance& army : province.get_armies()) {
			if (fought_today.contains(army.unique_id)) {
				continue;
			}
			// Cut off: no new men, the organisation falls; without organisation for ENCIRCLED_SURRENDER_DAYS days the army gives itself up
			if (army.is_encircled()) {
				fixed_point_t organisation = fixed_point_t::_0;
				for (RegimentInstance& regiment : army.get_regiment_instances()) {
					regiment.take_damage(std::min(regiment.get_organisation(), wd.encircled_org_loss_per_day.of(regiment.get_max_organisation())), fixed_point_t::_0);
					organisation += regiment.get_organisation();
				}
				if (organisation <= fixed_point_t::_0) {
					army.set_encircled_days(army.get_encircled_days() + 1);
					if (army.get_encircled_days() >= static_cast<std::uint32_t>(wd.encircled_surrender_days)) {
						surrendered.push_back(&army);
					}
				}
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
				supply = std::max(army.get_supply_ratio(), wd.recovery_min_supply.fixed());
			}
			for (RegimentInstance& regiment : army.get_regiment_instances()) {
				regiment.recover(
					regiment.get_max_organisation() * organisation_recovery * supply,
					friendly && supply >= wd.strength_recovery_min_supply.fixed() ? regiment.get_max_strength() * strength_recovery : fixed_point_t::_0
				);
			}
		}
	}
	for (ArmyInstance* army : surrendered) {
		instance_manager.get_notification_log().add(instance_manager.get_today(), &army->get_country(), "army_surrendered", army->get_name());
		instance_manager.get_unit_instance_manager().destroy_group(*army);
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
	void calculate_naval_side(NavalSide& side, OpenVicWarDefines const& wd) {
		const fixed_point_t min_org_factor = wd.min_organisation_factor.fixed();
		side.power = fixed_point_t::_0;
		side.organisation = fixed_point_t::_0;
		side.max_organisation = fixed_point_t::_0;
		side.ship_count = 0;

		for (NavyInstance* navy : side.navies) {
			// The ships that are not fully supplied shoot worse
			const fixed_point_t supply_level = navy->get_country().get_navy_supply_level();
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
				side.power += (ship.get_ship_type().gun_power + ship.get_ship_type().torpedo_attack) * strength_factor * organisation_factor
					* supply_level;
				side.organisation += ship.get_organisation();
				side.max_organisation += ship.get_max_organisation();
				side.ship_count++;
			}
		}
	}

	void apply_naval_damage(NavalSide& target, const fixed_point_t enemy_power, OpenVicWarDefines const& wd) {
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
				fixed_point_t ratio = average_power / hull / (fixed_point_t::_1 + ship.get_ship_type().evasion / wd.naval_evasion_divisor);
				ratio = std::min(ratio, fixed_point_t(wd.naval_max_damage_ratio));
				const int32_t rout_factor = ship.get_organisation() > fixed_point_t::_0 ? 1 : wd.rout_strength_multiplier;
				ship.take_damage(
					ship.get_max_organisation() * ratio * wd.naval_organisation_damage.numerator / Rate::DENOMINATOR,
					ship.get_max_strength() * ratio * wd.naval_strength_damage.numerator * rout_factor / Rate::DENOMINATOR
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
	OpenVicWarDefines const& wd = get_war_defines();
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

		calculate_naval_side(side_a, wd);
		calculate_naval_side(side_b, wd);
		if (side_a.ship_count == 0 || side_b.ship_count == 0) {
			continue;
		}
		const fixed_point_t power_a = side_a.power;
		const fixed_point_t power_b = side_b.power;
		apply_naval_damage(side_b, power_a, wd);
		apply_naval_damage(side_a, power_b, wd);
		calculate_naval_side(side_a, wd);
		calculate_naval_side(side_b, wd);

		const fixed_point_t rout_threshold = wd.rout_threshold.fixed();
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
				const fixed_point_t gain = fixed_point_t::_1 + fixed_point_t(static_cast<int32_t>(std::min<size_t>(loser.navies.size() * static_cast<size_t>(wd.naval_battle_score_ship_factor), static_cast<size_t>(wd.naval_battle_score_ship_cap)))) / wd.battle_score_regiment_divisor;
				fixed_point_t& score = war->is_attacker(winner_country) ? war->attacker_battle_score : war->defender_battle_score;
				score += gain;
				if (score > wd.max_battle_score) {
					score = fixed_point_t(wd.max_battle_score);
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
	OpenVicWarDefines const& wd = get_war_defines();
	const fixed_point_t base_attrition = wd.attrition_base.fixed();
	const fixed_point_t supply_attrition = wd.attrition_no_supply.fixed();
	// Attrition never kills a regiment: it stops at this share of the strength
	const fixed_point_t floor_share = wd.attrition_floor_strength.fixed();

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
			supply -= get_blockade_share(country) / wd.blockade_supply_divisor;
			if (supply < fixed_point_t::_0) {
				supply = fixed_point_t::_0;
			}

			fixed_point_t rate = fixed_point_t::_0;
			if (!friendly) {
				rate += base_attrition;
			}
			if (supply < fixed_point_t::_1) {
				rate += supply_attrition * (fixed_point_t::_1 - supply) * (friendly ? wd.attrition_friendly_multiplier.fixed() : fixed_point_t::_1);
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
				if (leader.get_unit_instance_group() == nullptr && leader.get_can_be_used() && !leader.is_marshal()
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
		// Generals command the armies of the countries with mechanic:command only
		if (country.has_army_command()) {
			assign_free(country.get_armies(), country.get_leaders<unit_branch_t::LAND>());
		}
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
	update_encirclements(instance_manager);
	update_scores(instance_manager);
	update_army_recovery(instance_manager);
	update_attrition(instance_manager);
	update_leaders(instance_manager);
	update_ai_peace(instance_manager);
	update_auto_plan(instance_manager);
	update_fronts(instance_manager);
}

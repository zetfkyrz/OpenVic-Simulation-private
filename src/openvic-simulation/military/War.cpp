#include "War.hpp"

#include "openvic-simulation/diagnostics/TickProfiler.hpp"
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
#include "openvic-simulation/military/LeaderSkills.hpp"
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
			// Not on board of a fleet and not at sea on its way by sea (P4d)
			if (army.get_transport_navy() == nullptr && !army.is_at_sea()) {
				result.push_back(&army);
			}
		}
		return result;
	}

	// Bonuses of a general from the personality and background traits
	struct LeaderBonus {
		fixed_point_t attack, defence, morale, attrition, reconnaissance;
	};

	LeaderBonus get_leader_bonus(LeaderInstance const* const leader, ModifierEffectCache const& cache) {
		LeaderBonus bonus;
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
			if (cache.get_reconnaissance() != nullptr) {
				bonus.reconnaissance += trait->get_effect(*cache.get_reconnaissance());
			}
		}
		return bonus;
	}

	LeaderBonus get_leader_bonus(ArmyInstance const& army, ModifierEffectCache const& cache) {
		return get_leader_bonus(army.get_leader(), cache);
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
	// A tired side gives in sooner: the war exhaustion of the leader of the side that is asked times PEACE_WAR_EXHAUSTION_FACTOR of Vic2 widens
	// the gap of the white peace and lowers the score it surrenders at
	memory::vector<CountryInstance*> const& asked_side = war.get_side(!proposer_is_attacker);
	const fixed_point_t tiredness = asked_side.empty() ? fixed_point_t::_0
		: asked_side.front()->get_war_exhaustion() * instance_manager.definition_manager.get_define_manager().get_ai_defines().get_peace_war_exhaustion_factor();

	switch (peace_type) {
	case peace_type_t::WHITE_PEACE:
		// The side that is clearly winning refuses, unless the war has dragged on for years or it is tired of it
		return other_score - proposer_score < fixed_point_t(pd.white_peace_score_gap) + tiredness || days > pd.white_peace_war_days;
	case peace_type_t::TAKE_STATES: {
		if (proposer_score >= fixed_point_t(pd.surrender_score) - tiredness) {
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

	restore_after_peace(instance_manager);
}

void WarManager::restore_after_peace(InstanceManager& instance_manager) {
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
				instance_manager.get_unit_instance_manager().kill_group(*army);
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
	// terrain_step x terrain_level: the skill of the general in the terrain makes its penalties smaller and its bonuses bigger
	RegimentType::OvBuff type_buff(
		RegimentType const& regiment_type, std::string_view terrain, std::vector<std::pair<RegimentType const*, std::uint32_t>> const& enemy_types,
		const std::uint32_t enemy_regiments, const fixed_point_t terrain_step = fixed_point_t::_0, const std::uint32_t terrain_level = 0
	) {
		RegimentType::OvBuff buff = regiment_type.ov.general;
		const auto add = [&buff](RegimentType::OvBuff const& other, const fixed_point_t share) {
			buff.attack_mult += other.attack_mult * share;
			buff.defense_mult += other.defense_mult * share;
			buff.breakthrough_mult += other.breakthrough_mult * share;
		};
		for (auto const& [identifier, terrain_buff] : regiment_type.ov.terrain) {
			if (identifier == terrain) {
				RegimentType::OvBuff scaled = terrain_buff;
				scaled.attack_mult = leader_skills::scale_terrain_effect(scaled.attack_mult, terrain_step, terrain_level);
				scaled.defense_mult = leader_skills::scale_terrain_effect(scaled.defense_mult, terrain_step, terrain_level);
				scaled.breakthrough_mult = leader_skills::scale_terrain_effect(scaled.breakthrough_mult, terrain_step, terrain_level);
				add(scaled, fixed_point_t::_1);
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
		fixed_point_t recon_advantage; // of the attackers, -1..1 (battle_formulas::recon_advantage)
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
	// The directions of the attack: the provinces the attacking divisions come from (each after the first widens their front and their damage)
	std::vector<ProvinceInstance const*> origins;
	for (BattleUnit const& unit : attackers.units) {
		ProvinceInstance const* const from = unit.origin != nullptr ? unit.origin : &province;
		if (std::find(origins.begin(), origins.end(), from) == origins.end()) {
			origins.push_back(from);
		}
	}
	const std::int32_t extra_directions = static_cast<std::int32_t>(
		std::min<std::size_t>(origins.size(), static_cast<std::size_t>(std::max(1, wd.battle_flank_max_directions)))
	) - 1;
	const std::uint32_t attacker_width = width + static_cast<std::uint32_t>(
		(fixed_point_t(static_cast<std::int32_t>(width)) * wd.battle_flank_width.fixed() * std::max(0, extra_directions)).floor<std::int32_t>()
	);
	const fixed_point_t flank_attack = fixed_point_t::_1 + wd.battle_flank_attack.fixed() * std::max(0, extra_directions);
	// The organisation of a division over its maximum
	const auto organisation_share = [](BattleUnit const& unit) -> fixed_point_t {
		fixed_point_t organisation = fixed_point_t::_0, max_organisation = fixed_point_t::_0;
		for (RegimentInstance const& regiment : unit.army->get_regiment_instances()) {
			if (regiment.get_division_id() == unit.division_id) {
				organisation += regiment.get_organisation();
				max_organisation += regiment.get_max_organisation();
			}
		}
		return max_organisation > fixed_point_t::_0 ? organisation / max_organisation : fixed_point_t::_0;
	};
	const auto choose_front = [&front_divisions, &organisation_share, &wd](BattleSideUnits& side, const std::uint32_t side_width) -> std::uint32_t {
		// A worn out division of yesterday's front goes to the reserve when a fresh one waits there
		bool fresh_reserve = false;
		for (BattleUnit const& unit : side.units) {
			if (std::find(front_divisions.begin(), front_divisions.end(), unit.division_id) == front_divisions.end()
				&& organisation_share(unit) >= wd.battle_rotate_reserve_organisation.fixed()) {
				fresh_reserve = true;
				break;
			}
		}
		std::uint32_t used = 0;
		for (const bool kept : { true, false }) {
			for (BattleUnit& unit : side.units) {
				const bool was_front = std::find(front_divisions.begin(), front_divisions.end(), unit.division_id) != front_divisions.end();
				if (unit.front || was_front != kept) {
					continue;
				}
				if (kept && fresh_reserve && organisation_share(unit) < wd.battle_rotate_organisation.fixed()) {
					continue;
				}
				if (used == 0 || used + unit.stats->width <= side_width) {
					unit.front = true;
					used += unit.stats->width;
				}
			}
		}
		return used;
	};
	const std::uint32_t attacker_ranks = choose_front(attackers, attacker_width);
	const std::uint32_t defender_ranks = choose_front(defenders, width);

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
	const fixed_point_t terrain_conditions = conditions.get_terrain(terrain_id);
	const fixed_point_t skill_step = wd.general_skill_step.fixed();
	// The reconnaissance of a side (section 12): its divisions in the battle, the front and the reserve, times 1 + the reconnaissance of the traits of
	// their generals
	const auto side_recon = [&cache](BattleSideUnits const& side) -> fixed_point_t {
		fixed_point_t sum = fixed_point_t::_0;
		for (BattleUnit const& unit : side.units) {
			sum += unit.stats->reconnaissance * std::max(fixed_point_t::_0, fixed_point_t::_1 + get_leader_bonus(*unit.army, cache).reconnaissance);
		}
		return sum;
	};
	const fixed_point_t recon_advantage = battle_formulas::recon_advantage(side_recon(attackers), side_recon(defenders));

	// What every division of the front is worth today: the parts of its types with their buffs, its general, its supply, the bonus of the planning of its
	// field marshal (P6)
	const auto evaluate = [&](BattleSideUnits& side, const bool attacking, std::vector<std::pair<RegimentType const*, std::uint32_t>> const& enemy_types,
		const std::uint32_t enemy_regiments) {
		for (BattleUnit& unit : side.units) {
			if (!unit.front) {
				continue;
			}
			const LeaderBonus bonus = get_leader_bonus(*unit.army, cache);
			// The skills of the general (section 7): the attack and the defence by levels, the terrain of the province
			LeaderInstance const* const general = unit.army->get_leader();
			const std::uint32_t attack_level = leader_skills::get_active_level(wd, general, leader_skills::skill_t::ATTACK);
			const std::uint32_t defence_level = leader_skills::get_active_level(wd, general, leader_skills::skill_t::DEFENCE);
			const std::uint32_t terrain_level = terrain != nullptr ? leader_skills::get_active_level(wd, general, leader_skills::skill_t::TERRAIN, terrain) : 0;
			const fixed_point_t skill_attack = fixed_point_t::_1 + skill_step * static_cast<std::int32_t>(attack_level);
			const fixed_point_t skill_defence = fixed_point_t::_1 + skill_step * static_cast<std::int32_t>(defence_level);
			const fixed_point_t leader_attack = std::max(wd.leader_min_multiplier.fixed(), fixed_point_t::_1 + bonus.attack / wd.leader_point_divisor);
			const fixed_point_t leader_defence = std::max(wd.leader_min_multiplier.fixed(), fixed_point_t::_1 + bonus.defence / wd.leader_point_divisor);
			// The supply level and the penalty of an army over the manpower of its country (both multiply the attack and the defence); a resting or
			// raiding army defends worse
			const fixed_point_t supply = unit.army->get_supply_level() * unit.army->get_country().manpower_combat_multiplier;
			// The model of the army (the laws, docs/LAWS_DESIGN.md): its attack and defence, and the bonus on the national land of the country
			CountryInstance const& army_country = unit.army->get_country();
			const fixed_point_t home_bonus = province.get_owner() == &army_country && province.is_owner_core()
				? army_country.get_law_effect(law_effect_t::army_home_bonus) : fixed_point_t::_0;
			const fixed_point_t law_attack = std::max(fixed_point_t::_0_10,
				fixed_point_t::_1 + army_country.get_law_effect(law_effect_t::army_attack) + home_bonus);
			const fixed_point_t law_defence = std::max(fixed_point_t::_0_10,
				fixed_point_t::_1 + army_country.get_law_effect(law_effect_t::army_defence) + home_bonus);
			const fixed_point_t stance_defence = unit.army->get_stance() == ArmyInstance::STANCE_REST || unit.army->get_stance() == ArmyInstance::STANCE_RAID
				? wd.stance_loose_defence.fixed() : fixed_point_t::_1;
			fixed_point_t attack = fixed_point_t::_0, defence = fixed_point_t::_0, breakthrough = fixed_point_t::_0;
			for (DivisionStats::TypePart const& part : unit.stats->parts) {
				const RegimentType::OvBuff buff = type_buff(*part.type, terrain_id, enemy_types, enemy_regiments, skill_step, terrain_level);
				attack += part.attack * (fixed_point_t::_1 + buff.attack_mult);
				defence += part.defence * (fixed_point_t::_1 + buff.defense_mult);
				breakthrough += part.breakthrough * (fixed_point_t::_1 + buff.breakthrough_mult);
			}
			const fixed_point_t planning = fixed_point_t::_1 + get_army_planning(*unit.army);
			const fixed_point_t tactics = battle_formulas::recon_tactics(wd, attacking ? recon_advantage : -recon_advantage);
			unit.attack = attack * leader_attack * skill_attack * supply * planning * tactics * law_attack;
			unit.breakthrough = breakthrough * leader_attack * supply * planning * tactics * law_attack;
			if (attacking) {
				unit.protection = unit.breakthrough;
				// Over a river: the army came from a province whose border with this one runs along a river
				ProvinceInstance const* const from = unit.origin;
				const bool river = from != nullptr
					&& map_definition.get_river_border_share(from->province_definition, province.province_definition) >= wd.river_border_min_share.fixed();
				// The terrain for an attacker: its conditions (a penalty or a bonus) and the defence the terrain gives the enemy (a penalty), both
				// changed by the skill of the general in it
				const fixed_point_t own_conditions = fixed_point_t::_1
					+ leader_skills::scale_terrain_effect(terrain_conditions - fixed_point_t::_1, skill_step, terrain_level);
				const fixed_point_t enemy_terrain = fixed_point_t::_1
					- leader_skills::scale_terrain_effect(fixed_point_t::_1 - terrain_defence, skill_step, terrain_level);
				const fixed_point_t terrain_factor = enemy_terrain > fixed_point_t::_0 ? terrain_defence / enemy_terrain : fixed_point_t::_1;
				unit.conditions = own_conditions * terrain_factor * conditions.night * conditions.weather
					* (river ? conditions.river_crossing : fixed_point_t::_1) * flank_attack;
			} else {
				// The reconnaissance of the attackers finds the works of the dig in (section 12)
				const fixed_point_t fortification = unit.stats->fortification
					+ battle_formulas::recon_dig_in(wd, fixed_point_t(static_cast<std::int32_t>(unit.army->get_dig_in_level())), recon_advantage);
				// The defence the terrain gives is a bonus: the skill of the general makes it bigger
				const fixed_point_t own_terrain_defence = fixed_point_t::_1
					+ leader_skills::scale_terrain_effect(terrain_defence - fixed_point_t::_1, skill_step, terrain_level);
				unit.protection = battle_formulas::defender_protection(
					wd, defence * leader_defence * skill_defence * supply * stance_defence * law_defence, fortification, own_terrain_defence
				);
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
	const auto strike = [&wd](
		BattleSideUnits& targets, const std::uint32_t target_ranks, const fixed_point_t attack, const fixed_point_t side_conditions, const bool attacking
	) {
		for (BattleUnit& unit : targets.units) {
			if (unit.front && target_ranks > 0) {
				const fixed_point_t directed = attack * static_cast<std::int32_t>(unit.stats->width) / static_cast<std::int32_t>(target_ranks);
				unit.hit = battle_formulas::battle_hit(wd, directed, side_conditions, unit.stats->density, unit.protection, unit.stats->casualties_taken);
				if (attacking) {
					unit.hit.strength_loss = wd.battle_attacker_strength_loss_mult.of(unit.hit.strength_loss);
					unit.hit.organisation_loss = wd.battle_attacker_organisation_loss_mult.of(unit.hit.organisation_loss);
				}
			}
		}
	};
	strike(defenders, defender_ranks, attacker_attack, attacker_conditions, false);
	strike(attackers, attacker_ranks, defender_attack, fixed_point_t::_1, true);

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
		day.recon_advantage = recon_advantage;
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
	InstanceManager const& instance_manager, std::span<ArmyInstance const* const> attacking_armies, ProvinceInstance const& target,
	std::span<ProvinceInstance const* const> origins
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
		for (std::size_t army_index = 0; army_index < side.armies.size(); ++army_index) {
			ArmyInstance* const army = side.armies[army_index];
			ProvinceInstance const* const given_origin = attacking && army_index < origins.size() ? origins[army_index] : nullptr;
			for (DivisionInstance const& division : army->get_divisions()) {
				DivisionStats const& stats = get_live_division_stats(instance_manager, *army, division);
				if (stats.regiments == 0) {
					continue;
				}
				BattleUnit unit;
				unit.army = army;
				unit.division_id = division.id;
				unit.stats = &stats;
				unit.origin = !attacking ? nullptr : given_origin != nullptr ? given_origin
					: &army->get_location() != &target ? &army->get_location() : army->get_previous_location();
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
	// The chance is what the attacker sees: with a worse reconnaissance it takes the defence as lasting longer (section 12)
	estimate.recon_advantage = day.recon_advantage;
	estimate.win_chance = battle_formulas::win_chance(
		estimate.attacker_days, defence_ends * battle_formulas::recon_caution(wd, day.recon_advantage), wd.battle_win_steepness
	);
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
		// The generals of both sides learn (section 7): the attack or the defence, and the terrain of the province
		const auto learn = [&](BattleSideUnits const& side, const leader_skills::skill_t skill) {
			for (ArmyInstance* army : side.armies) {
				LeaderInstance* const general = army->get_leader();
				if (general == nullptr || general->is_marshal()) {
					continue;
				}
				leader_skills::add_experience(instance_manager, *general, skill, wd.general_xp_per_battle_day);
				if (province.get_terrain_type() != nullptr) {
					leader_skills::add_experience(
						instance_manager, *general, leader_skills::skill_t::TERRAIN, wd.general_terrain_xp_share.of(wd.general_xp_per_battle_day),
						province.get_terrain_type()
					);
				}
			}
		};
		learn(attackers, leader_skills::skill_t::ATTACK);
		learn(defenders, leader_skills::skill_t::DEFENCE);
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
		report.recon_advantage = battle_day.recon_advantage;
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
			// A lost battle costs every country of the losers BATTLE_LOST_PRESTIGE_PER_REGIMENT prestige for each of its regiments in it (and war exhaustion)
			std::vector<std::pair<CountryInstance*, std::int32_t>> loser_countries;
			for (BattleUnit const& unit : loser.units) {
				CountryInstance* const country = &unit.army->get_country();
				auto it = std::find_if(loser_countries.begin(), loser_countries.end(), [country](auto const& entry) -> bool { return entry.first == country; });
				if (it == loser_countries.end()) {
					loser_countries.emplace_back(country, static_cast<std::int32_t>(unit.stats->regiments));
				} else {
					it->second += static_cast<std::int32_t>(unit.stats->regiments);
				}
			}
			for (auto const& [country, regiments] : loser_countries) {
				const fixed_point_t lost = wd.battle_lost_prestige_per_regiment * std::max(1, regiments);
				country->add_prestige(-lost);
				country->war_prestige_lost_today += lost;
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
						unit_manager.kill_group(*mover);
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

bool WarManager::is_home_land(CountryInstance const& country, ProvinceInstance const& province) const {
	const auto it = supply_reach.find(&country);
	if (it == supply_reach.end()) {
		return true;
	}
	const std::size_t index = type_safe::get(province.index);
	return index < it->second.size() && it->second[index] == 1;
}

void WarManager::update_encirclements(InstanceManager& instance_manager) {
	MapInstance& map_instance = instance_manager.get_map_instance();
	CountryInstanceManager const& countries = instance_manager.get_country_instance_manager();
	const std::size_t count = map_instance.get_province_instances().size();
	supply_reach.clear();
	std::deque<ProvinceInstance const*> queue;
	std::vector<std::uint8_t> peace_reach;
	// Over land and over the straits (Constantinople and Anatolia, Denmark and Scania...)
	const auto connects = [](ProvinceDefinition::adjacency_t const& adjacency) -> bool {
		return !adjacency.get_to().is_water() && (adjacency.get_type() == ProvinceDefinition::adjacency_t::type_t::LAND
			|| adjacency.get_type() == ProvinceDefinition::adjacency_t::type_t::STRAIT);
	};
	const auto has_naval_base = [](ProvinceInstance const& province) -> bool {
		for (BuildingInstance const& building : province.get_buildings()) {
			if (building.building_type.get_identifier() == "naval_base" && type_safe::get(building.get_level()) > 0) {
				return true;
			}
		}
		return false;
	};
	for (CountryInstance& country : instance_manager.get_country_instance_manager().get_country_instances()) {
		if (!country.exists() || !country.has_armies()) {
			continue;
		}
		const bool at_war = country.is_at_war();
		const SupplyFriends friends = SupplyFriends::build(country, countries, *this);
		// 1: reached over land from the capital; 2: by the sea, from a port (ov_supply.SUPPLY_SEA_LOSS of the supply is lost on the way). A country at
		// peace is never cut off: it only needs to know which of its armies are supplied by the sea
		std::vector<std::uint8_t>& reach = at_war ? supply_reach[&country] : peace_reach;
		reach.assign(count, 0);
		queue.clear();
		bool any_source = false;
		const auto add_source = [&](ProvinceInstance const& province, const std::uint8_t mark) {
			const std::size_t index = type_safe::get(province.index);
			if (reach[index] == 0) {
				reach[index] = mark;
				queue.push_back(&province);
				any_source = true;
			}
		};
		const auto spread = [&](const std::uint8_t mark) {
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
						reach[index] = mark;
						queue.push_back(&next);
					}
				}
			}
		};
		// The land: from the capital (held by a friend)
		if (country.get_capital() != nullptr && is_supply_held(*country.get_capital(), friends)) {
			add_source(*country.get_capital(), 1);
			spread(1);
		}
		// The sea: the ports the country holds that no enemy blockades and the land does not reach (an island, a colony, a province cut off from the rest
		// of the country). A port with a naval base always; any other port when the land reached from the capital has a free port the ships sail from
		bool home_port = false;
		for (ProvinceInstance* province : country.get_owned_provinces()) {
			if (reach[type_safe::get(province->index)] == 1 && province->get_controller() == &country && province->province_definition.has_port()
				&& !is_blockaded(*province)) {
				home_port = true;
				break;
			}
		}
		// (the ports it took from the enemy too: a landing is supplied through the port it took, version 81)
		for (ProvinceInstance* province : country.get_controlled_provinces()) {
			if (!province->province_definition.has_port() || is_blockaded(*province) || reach[type_safe::get(province->index)] != 0) {
				continue;
			}
			if (home_port || (province->get_owner() == &country && has_naval_base(*province))) {
				add_source(*province, 2);
			}
		}
		spread(2);
		// How an army is reached: in its province, or next to it (it stands on the land of the enemy at the edge of ours: the last step leads to it);
		// over land when any way is over land
		const auto army_reach = [&](ArmyInstance const& army) -> std::uint8_t {
			ProvinceInstance const& at = army.get_location();
			if (army.get_transport_navy() != nullptr || at.province_definition.is_water()) {
				return 1;
			}
			const std::uint8_t here = reach[type_safe::get(at.index)];
			if (here != 0) {
				return here;
			}
			// Held land the supply does not reach: only over the beach (below); the land of the enemy: also next to the land reached
			const bool held = is_supply_held(at, friends);
			std::uint8_t best = 0;
			bool free_sea = false;
			for (ProvinceDefinition::adjacency_t const& adjacency : at.province_definition.get_adjacencies()) {
				if (adjacency.get_to().is_water()) {
					// A sea next to the army with no navy of an enemy at war (the supply over the beach of a landing, version 81)
					ProvinceInstance const& sea = map_instance.get_province_instance_by_definition(adjacency.get_to());
					free_sea |= std::none_of(sea.get_navies().begin(), sea.get_navies().end(), [&country](NavyInstance const& navy) -> bool {
						return !navy.empty() && navy.get_country().is_at_war_with(country);
					});
					continue;
				}
				if (held || !connects(adjacency)) {
					continue;
				}
				const std::uint8_t next = reach[type_safe::get(map_instance.get_province_instance_by_definition(adjacency.get_to()).index)];
				if (next == 1) {
					return 1;
				}
				best = std::max(best, next);
			}
			// An army on the coast of the enemy (a landing) is supplied over the beach by the sea when the country has a free port of its own
			if (best == 0 && free_sea && home_port) {
				return 2;
			}
			return best;
		};
		// A country without any source (its capital and its ports are lost) is not cut off as a whole: its armies keep what they have
		if (!any_source) {
			if (at_war) {
				supply_reach.erase(&country);
			}
			for (ArmyInstance& army : country.get_armies()) {
				army.set_cut_off(false);
				army.set_encircled(false);
				army.set_supply_by_sea(false);
			}
			continue;
		}
		if (!at_war) {
			for (ArmyInstance& army : country.get_armies()) {
				army.set_cut_off(false);
				army.set_encircled(false);
				army.set_encircled_days(0);
				army.set_supply_by_sea(army_reach(army) == 2);
			}
			continue;
		}
		// The pockets: the land of the friends that is not reached, by its connected parts; a part next to the land of an enemy at war is a pocket (the
		// others, a colony far from the war, are only cut off from the supply)
		const auto enemy_held = [&](ProvinceInstance const& province) -> bool {
			CountryInstance const* const controller = province.get_controller();
			return controller != nullptr && controller != &country && country.is_at_war_with(*controller);
		};
		std::vector<std::int32_t> part(count, -1);
		std::vector<std::uint8_t> part_touches_enemy;
		const auto pocket_of = [&](ProvinceInstance const& start) -> bool {
			const std::size_t start_index = type_safe::get(start.index);
			if (part[start_index] >= 0) {
				return part_touches_enemy[static_cast<std::size_t>(part[start_index])] != 0;
			}
			const std::int32_t id = static_cast<std::int32_t>(part_touches_enemy.size());
			part_touches_enemy.push_back(0);
			std::deque<ProvinceInstance const*> pocket_queue { &start };
			part[start_index] = id;
			while (!pocket_queue.empty()) {
				ProvinceInstance const* const current = pocket_queue.front();
				pocket_queue.pop_front();
				for (ProvinceDefinition::adjacency_t const& adjacency : current->province_definition.get_adjacencies()) {
					if (!connects(adjacency)) {
						continue;
					}
					ProvinceInstance const& next = map_instance.get_province_instance_by_definition(adjacency.get_to());
					const std::size_t index = type_safe::get(next.index);
					if (enemy_held(next)) {
						part_touches_enemy[static_cast<std::size_t>(id)] = 1;
					} else if (part[index] < 0 && reach[index] == 0 && is_supply_held(next, friends)) {
						part[index] = id;
						pocket_queue.push_back(&next);
					}
				}
			}
			return part_touches_enemy[static_cast<std::size_t>(id)] != 0;
		};
		for (ArmyInstance& army : country.get_armies()) {
			ProvinceInstance const& at = army.get_location();
			const std::uint8_t got = army_reach(army);
			const bool reached = got != 0;
			// Cut off: no supply; in a pocket (deep in the land of the enemy, or on the land of the friends that touches the enemy): it loses organisation too
			const bool pocket = !reached && (!is_supply_held(at, friends) || pocket_of(at));
			army.set_cut_off(!reached);
			army.set_encircled(pocket);
			army.set_supply_by_sea(got == 2);
			if (!pocket) {
				army.set_encircled_days(0);
			}
		}
	}
}

namespace {
	// What the regiments of a country want today: the strength of each, the men and the goods of all of them
	struct ReinforcementNeed {
		std::vector<std::pair<RegimentInstance*, fixed_point_t>> regiments;
		std::int64_t men = 0;
		std::vector<std::pair<GoodDefinition const*, fixed_point_t>> goods;
	};

	void add_good_need(std::vector<std::pair<GoodDefinition const*, fixed_point_t>>& goods, GoodDefinition const* good, const fixed_point_t quantity) {
		for (auto& entry : goods) {
			if (entry.first == good) {
				entry.second += quantity;
				return;
			}
		}
		goods.emplace_back(good, quantity);
	}
}

void WarManager::update_army_recovery(InstanceManager& instance_manager) {
	OpenVicWarDefines const& wd = get_war_defines();
	const fixed_point_t organisation_recovery = wd.organisation_recovery.fixed();
	const fixed_point_t strength_recovery = wd.strength_recovery.fixed();
	std::vector<ArmyInstance*> surrendered;
	std::unordered_map<CountryInstance*, ReinforcementNeed> needs;
	// The mobilised regiments get their new men from the mobilisation pool, not from the manpower of the soldiers
	std::unordered_map<CountryInstance*, ReinforcementNeed> mobilised_needs;
	ModifierEffectCache const& cache = instance_manager.definition_manager.get_modifier_manager().get_modifier_effect_cache();

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

			// Men are only replaced on friendly ground, not when the army is cut off from the supply
			CountryInstance const* const owner = province.get_owner();
			const bool friendly = !army.is_cut_off() && owner != nullptr
				&& (owner == &army.get_country() || army.get_country().has_alliance_with(*owner));

			// Armies the national stockpile cannot supply recover more slowly, and get no new men below half supplies
			// (only at war, and never below a quarter, so that a country with an empty stockpile still can recover)
			fixed_point_t supply = fixed_point_t::_1;
			if (army.get_country().is_at_war()) {
				supply = std::max(army.get_supply_ratio(), wd.recovery_min_supply.fixed());
			}
			const bool new_men = friendly && supply >= wd.strength_recovery_min_supply.fixed();
			// The technologies (reinforce_rate) make the new men come faster
			fixed_point_t reinforce_speed = strength_recovery;
			if (new_men && cache.get_reinforce_rate() != nullptr) {
				reinforce_speed *= std::max(fixed_point_t::_0, fixed_point_t::_1 + army.get_country().get_modifier_effect_value(*cache.get_reinforce_rate()));
			}
			ReinforcementNeed* need = nullptr;
			ReinforcementNeed* mobilised_need = nullptr;
			const std::int32_t rest = army.get_stance() == ArmyInstance::STANCE_REST ? std::max(1, wd.stance_rest_recovery) : 1;
			for (RegimentInstance& regiment : army.get_regiment_instances()) {
				regiment.recover(regiment.get_max_organisation() * organisation_recovery * supply * rest, fixed_point_t::_0);
				const fixed_point_t gain = new_men
					? std::min(regiment.get_max_strength() * reinforce_speed, regiment.get_max_strength() - regiment.get_strength())
					: fixed_point_t::_0;
				if (gain <= fixed_point_t::_0) {
					continue;
				}
				if (regiment.is_mobilised()) {
					if (mobilised_need == nullptr) {
						mobilised_need = &mobilised_needs[&army.get_country()];
					}
				} else if (need == nullptr) {
					need = &needs[&army.get_country()];
				}
				ReinforcementNeed* const regiment_need = regiment.is_mobilised() ? mobilised_need : need;
				regiment_need->regiments.emplace_back(&regiment, gain);
				regiment_need->men += (gain * wd.template_men_per_strength).ceil<std::int64_t>();
				if (regiment.get_max_strength() > fixed_point_t::_0) {
					const fixed_point_t share = wd.reinforce_goods_share.of(gain / regiment.get_max_strength());
					for (auto const& [good, quantity] : regiment.unit_type.get_build_cost()) {
						add_good_need(regiment_need->goods, good, quantity * share);
					}
				}
			}
		}
	}

	// The new men come out of the free manpower of the country and cost the goods of the regiments for their share of the strength: when the
	// country has not enough men, every regiment gets the same share of what it wants, and not more than the share of its scarcest good (the order
	// of the regiments does not matter)
	UnitInstanceManager& unit_manager = instance_manager.get_unit_instance_manager();
	for (CountryInstance& country : instance_manager.get_country_instance_manager().get_country_instances()) {
		country.reinforce_wanted_men = 0;
		country.reinforce_given_men = 0;
		country.reinforce_limit = 0;
		country.mobilised_reinforce_wanted_men = 0;
		country.mobilised_reinforce_given_men = 0;
		country.mobilised_reinforce_limit = 0;
	}
	// The share of the men is the same for all the regiments of the country (of its pool: the soldiers, or the mobilisation)
	const auto reinforce = [&wd](CountryInstance* country, ReinforcementNeed const& need, const std::int64_t free_men, std::int64_t& wanted_men,
		std::int64_t& given_men, std::int32_t& limit) -> void {
		fixed_point_t ratio = fixed_point_t::_1;
		wanted_men = need.men;
		if (country->is_rebel_country()) {
			for (auto const& [regiment, gain] : need.regiments) {
				regiment->recover(fixed_point_t::_0, gain);
			}
			given_men = need.men;
			return;
		}
		if (free_men < need.men) {
			ratio = fixed_point_t(static_cast<int32_t>(free_men * 10000 / need.men)) / 10000;
			limit |= 1;
		}
		// The share of every good (what the stockpile has of what all the regiments want of it); a regiment gets the smallest share of its own
		// goods: the wine of the cavalry does not hold back the infantry
		std::vector<std::pair<GoodDefinition const*, fixed_point_t>> good_shares;
		for (auto const& [good, quantity] : need.goods) {
			const fixed_point_t stockpile = std::max(fixed_point_t::_0, country->get_good_data(*good).stockpile_amount);
			good_shares.emplace_back(good, quantity > stockpile && quantity > fixed_point_t::_0 ? stockpile / quantity : fixed_point_t::_1);
		}
		const auto good_share = [&good_shares](GoodDefinition const* good) -> fixed_point_t {
			for (auto const& [entry_good, share] : good_shares) {
				if (entry_good == good) {
					return share;
				}
			}
			return fixed_point_t::_1;
		};
		fixed_point_t given = fixed_point_t::_0;
		for (auto const& [regiment, gain] : need.regiments) {
			fixed_point_t regiment_ratio = ratio;
			for (auto const& [good, quantity] : regiment->unit_type.get_build_cost()) {
				if (quantity > fixed_point_t::_0 && good_share(good) < regiment_ratio) {
					regiment_ratio = good_share(good);
					limit |= 2;
				}
			}
			if (regiment_ratio <= fixed_point_t::_0) {
				continue;
			}
			const fixed_point_t got = gain * regiment_ratio;
			regiment->recover(fixed_point_t::_0, got);
			given += got;
			if (regiment->get_max_strength() > fixed_point_t::_0) {
				const fixed_point_t share = wd.reinforce_goods_share.of(got / regiment->get_max_strength());
				for (auto const& [good, quantity] : regiment->unit_type.get_build_cost()) {
					CountryInstance::good_data_t& good_data = country->get_good_data(*good);
					good_data.stockpile_amount -= quantity * share;
					good_data.stockpile_change_yesterday -= quantity * share;
				}
			}
		}
		given_men = std::min(need.men, (given * wd.template_men_per_strength).floor<std::int64_t>());
	};
	for (auto& [country, need] : needs) {
		const std::int64_t free_men = country->is_rebel_country() ? 0 : std::max<std::int64_t>(0, unit_manager.get_manpower(*country).free_men);
		reinforce(country, need, free_men, country->reinforce_wanted_men, country->reinforce_given_men, country->reinforce_limit);
	}
	// After the regular regiments of the country (they share its stockpile): one country at a time, so the order of the map does not matter
	for (auto& [country, need] : mobilised_needs) {
		const std::int64_t free_men = country->is_rebel_country() ? 0 : std::max<std::int64_t>(0, unit_manager.get_mobilisation(*country).free_men);
		reinforce(
			country, need, free_men, country->mobilised_reinforce_wanted_men, country->mobilised_reinforce_given_men, country->mobilised_reinforce_limit
		);
	}
	for (ArmyInstance* army : surrendered) {
		instance_manager.get_notification_log().add(instance_manager.get_today(), &army->get_country(), "army_surrendered", army->get_name());
		instance_manager.get_unit_instance_manager().kill_group(*army);
	}
}

namespace {
	struct NavalSide {
		std::vector<NavyInstance*> navies;
		fixed_point_t power;
		fixed_point_t organisation;
		fixed_point_t max_organisation;
		size_t ship_count = 0;
		size_t light_ships = 0; // not capital, not transports: they screen the capital ships

		fixed_point_t get_organisation_ratio() const {
			return max_organisation > fixed_point_t::_0 ? organisation / max_organisation : fixed_point_t::_0;
		}
	};

	// Share of the maximum strength / organisation of a ship that is lost each day by a ship hit with the force
	// of its own hull
	// The attack and the defence of the admiral of a fleet (his traits, as for a general)
	fixed_point_t admiral_multiplier(NavyInstance const& navy, ModifierEffectCache const& cache, OpenVicWarDefines const& wd, const bool attack) {
		const LeaderBonus bonus = get_leader_bonus(navy.get_leader(), cache);
		return std::max(wd.leader_min_multiplier.fixed(), fixed_point_t::_1 + (attack ? bonus.attack : bonus.defence) / wd.leader_point_divisor);
	}

	void calculate_naval_side(NavalSide& side, OpenVicWarDefines const& wd, ModifierEffectCache const& cache) {
		const fixed_point_t min_org_factor = wd.min_organisation_factor.fixed();
		side.power = fixed_point_t::_0;
		side.organisation = fixed_point_t::_0;
		side.max_organisation = fixed_point_t::_0;
		side.ship_count = 0;
		side.light_ships = 0;

		for (NavyInstance* navy : side.navies) {
			// The ships that are not fully supplied shoot worse; the admiral makes them shoot better
			const fixed_point_t supply_level = navy->get_country().get_navy_supply_level() * admiral_multiplier(*navy, cache, wd, true);
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
				side.light_ships += !ship.get_ship_type().is_capital && !ship.get_ship_type().is_transport;
			}
		}
	}

	void apply_naval_damage(NavalSide& target, const fixed_point_t enemy_power, OpenVicWarDefines const& wd, ModifierEffectCache const& cache) {
		if (target.ship_count == 0) {
			return;
		}
		// The light ships screen the capital ones: these take NAVAL_SCREENED_DAMAGE of their share, the light ones the rest (the total is the same)
		const std::int32_t ships = static_cast<std::int32_t>(target.ship_count);
		const std::int32_t light = static_cast<std::int32_t>(target.light_ships);
		std::int32_t capital = 0;
		for (NavyInstance* navy : target.navies) {
			for (ShipInstance const& ship : navy->get_ship_instances()) {
				capital += ship.get_strength() > fixed_point_t::_0 && ship.get_ship_type().is_capital;
			}
		}
		const fixed_point_t average_power = enemy_power / ships;
		const bool screened = light > 0 && capital > 0;
		const fixed_point_t capital_power = screened ? wd.naval_screened_damage.of(average_power) : average_power;
		const fixed_point_t light_power = screened
			? average_power + (average_power - capital_power) * capital / light : average_power;

		for (NavyInstance* navy : target.navies) {
			const fixed_point_t defence = admiral_multiplier(*navy, cache, wd, false);
			for (ShipInstance& ship : navy->get_ship_instances()) {
				if (ship.get_strength() <= fixed_point_t::_0) {
					continue;
				}
				const fixed_point_t ship_power = ship.get_ship_type().is_capital ? capital_power
					: (!ship.get_ship_type().is_transport ? light_power : average_power);
				// A thin hull takes the full force, a thick one a part of it (at most 6 times the damage); evasion avoids some hits; the admiral defends
				const fixed_point_t hull = std::max(ship.get_ship_type().hull, fixed_point_t::_1);
				fixed_point_t ratio = ship_power / hull / (fixed_point_t::_1 + ship.get_ship_type().evasion / wd.naval_evasion_divisor) / defence;
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
	ModifierEffectCache const& cache = instance_manager.definition_manager.get_modifier_manager().get_modifier_effect_cache();
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

		calculate_naval_side(side_a, wd, cache);
		calculate_naval_side(side_b, wd, cache);
		if (side_a.ship_count == 0 || side_b.ship_count == 0) {
			continue;
		}
		const fixed_point_t power_a = side_a.power;
		const fixed_point_t power_b = side_b.power;
		apply_naval_damage(side_b, power_a, wd, cache);
		apply_naval_damage(side_a, power_b, wd, cache);
		calculate_naval_side(side_a, wd, cache);
		calculate_naval_side(side_b, wd, cache);

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
			// Transports do not blockade
			bool warships = false;
			for (ShipInstance const& ship : navy.get_ship_instances()) {
				warships |= !ship.get_ship_type().is_transport && ship.get_strength() > fixed_point_t::_0;
			}
			if (!warships) {
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
	for (CountryInstance& country : instance_manager.get_country_instance_manager().get_country_instances()) {
		country.blockade_share = get_blockade_share(country);
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
			// Only countries at war need supplies (the supply level of the army: its reserves keep it while the supplies do not arrive), a
			// blockade of the ports cuts them
			fixed_point_t supply = country.is_at_war() ? army.get_supply_level() : fixed_point_t::_1;
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
			// More regiments than the province feeds (at war, or on the land of others: at peace at home the regiments are in their barracks)
			if (army.get_supply_load_ratio() > fixed_point_t::_1 && (country.is_at_war() || !friendly)) {
				rate += std::min(wd.attrition_overload_max.fixed(), wd.attrition_overload.of(army.get_supply_load_ratio() - fixed_point_t::_1));
			}
			if (rate <= fixed_point_t::_0) {
				continue;
			}

			// Modifiers of the country (technology, ...) and the general change the attrition; a raiding army forages on the land of an enemy
			fixed_point_t multiplier = fixed_point_t::_1;
			if (army.get_stance() == ArmyInstance::STANCE_RAID && owner != nullptr && country.is_at_war_with(*owner)) {
				rate = wd.stance_raid_attrition.of(rate);
			}
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

std::string_view WarManager::get_justify_block_reason(CountryInstance const& country, CountryInstance const& target, const Date today) const {
	if (&country == &target || !target.exists() || target.is_rebel_country()) {
		return "Нельзя обосновать войну против этой страны";
	}
	if (country.is_at_war_with(target)) {
		return "Уже воюем";
	}
	if (country.has_alliance_with(target)) {
		return "Союзник";
	}
	if (country.has_casus_belli_against(target, today)) {
		return "Повод для войны уже есть";
	}
	if (country.justify_target != nullptr) {
		return "Уже обосновываем войну против другой страны (одно обоснование за раз)";
	}
	return {};
}

bool WarManager::start_justification(CountryInstance& country, CountryInstance& target, const Date today) {
	if (!get_justify_block_reason(country, target, today).empty()) {
		return false;
	}
	country.justify_target = &target;
	country.justify_days_left = get_war_defines().justify_war_days;
	return true;
}

bool WarManager::declare_war_as_country(
	InstanceManager& instance_manager, CountryInstance& attacker, CountryInstance& defender, WarGoal const& goal
) {
	const Date today = instance_manager.get_today();
	const bool justified = attacker.has_casus_belli_against(defender, today);
	if (!declare_war(instance_manager, attacker, defender, goal)) {
		return false;
	}
	if (justified) {
		std::erase_if(attacker.casus_belli, [&defender](auto const& entry) -> bool { return entry.first == &defender; });
	} else {
		attacker.change_infamy(get_war_defines().no_cb_war_infamy);
		attacker.change_war_exhaustion(get_war_defines().no_cb_war_exhaustion);
	}
	if (attacker.justify_target == &defender) {
		attacker.justify_target = nullptr;
		attacker.justify_days_left = 0;
	}
	return true;
}

void WarManager::update_justifications(InstanceManager& instance_manager) {
	OpenVicWarDefines const& wd = get_war_defines();
	ModifierEffectCache const& cache = instance_manager.definition_manager.get_modifier_manager().get_modifier_effect_cache();
	const Date today = instance_manager.get_today();
	const bool month_start = today.is_month_start();
	for (CountryInstance& country : instance_manager.get_country_instance_manager().get_country_instances()) {
		if (country.justify_target != nullptr) {
			CountryInstance& target = *country.justify_target;
			if (!target.exists() || country.has_alliance_with(target) || country.is_at_war_with(target)) {
				country.justify_target = nullptr;
				country.justify_days_left = 0;
			} else if (--country.justify_days_left <= 0) {
				country.casus_belli.emplace_back(&target, today + Timespan::from_days(wd.casus_belli_days));
				country.change_infamy(wd.justify_war_infamy);
				instance_manager.get_notification_log().add(today, &country, "casus_belli_ready", target.get_identifier(), &target);
				country.justify_target = nullptr;
				country.justify_days_left = 0;
			}
		}
		std::erase_if(country.casus_belli, [today](auto const& entry) -> bool { return entry.second < today || !entry.first->exists(); });
		if (month_start && cache.get_badboy() != nullptr && country.exists()) {
			const fixed_point_t change = country.get_modifier_effect_value(*cache.get_badboy());
			if (change != fixed_point_t::_0) {
				country.change_infamy(change);
			}
		}
	}
}

War const* WarManager::get_war_to_call_into(CountryInstance const& caller) const {
	War const* first = nullptr;
	for (War const& war : wars) {
		if (!war.has_participant(caller)) {
			continue;
		}
		memory::vector<CountryInstance*> const& side = war.get_side(war.is_attacker(caller));
		if (!side.empty() && side.front() == &caller) {
			return &war;
		}
		if (first == nullptr) {
			first = &war;
		}
	}
	return first;
}

bool WarManager::can_join_war(War const& war, const bool attacker_side, CountryInstance const& country) const {
	if (!country.exists() || country.is_rebel_country() || war.has_participant(country)) {
		return false;
	}
	for (CountryInstance const* enemy : war.get_side(!attacker_side)) {
		if (country.has_alliance_with(*enemy)) {
			return false;
		}
	}
	return true;
}

bool WarManager::would_join_war(War const& war, CountryInstance const& country) const {
	if (!country.is_ai()) {
		return true; // the player called by the AI: he is told and leaves the war by a separate peace if he does not want it
	}
	// An AI country does not open a second war and does not join while it is tired
	for (War const& other : wars) {
		if (&other != &war && other.has_participant(country)) {
			return false;
		}
	}
	return country.get_war_exhaustion() <= get_war_defines().call_ally_max_exhaustion;
}

bool WarManager::join_war(InstanceManager& instance_manager, const std::uint32_t war_id, CountryInstance& country, const bool attacker_side) {
	War* const war = find_war(war_id);
	if (war == nullptr || !can_join_war(*war, attacker_side, country)) {
		return false;
	}
	(attacker_side ? war->attackers : war->defenders).push_back(&country);
	for (CountryInstance* enemy : war->get_side(!attacker_side)) {
		country.set_at_war_with(*enemy, true);
	}
	CountryInstance* const leader = war->get_side(attacker_side).front();
	instance_manager.get_notification_log().add(instance_manager.get_today(), leader, "ally_joined_war", country.get_identifier(), &country);
	instance_manager.get_notification_log().add(instance_manager.get_today(), &country, "joined_war", leader->get_identifier(), leader);
	SPDLOG_INFO("War {}: {} joins the side of {}", war_id, country.get_identifier(), leader->get_identifier());
	return true;
}

std::string_view WarManager::get_separate_peace_block_reason(
	InstanceManager const& instance_manager, War const& war, CountryInstance const& country
) const {
	if (!war.has_participant(country)) {
		return "Страна не участвует в этой войне";
	}
	const bool attacker_side = war.is_attacker(country);
	memory::vector<CountryInstance*> const& side = war.get_side(attacker_side);
	memory::vector<CountryInstance*> const& other_side = war.get_side(!attacker_side);
	if (side.empty() || side.front() == &country || other_side.empty()) {
		return "Лидер стороны заключает мир за всю сторону — отдельный мир только для союзников";
	}
	CountryInstance const* const other_leader = other_side.front();
	if (!other_leader->is_ai()) {
		return {};
	}
	// The AI leader lets go a country that holds nothing of his side, or one whose whole side he would give a white peace
	bool holds_enemy_land = false;
	for (CountryInstance const* enemy : other_side) {
		for (ProvinceInstance const* province : enemy->get_owned_provinces()) {
			holds_enemy_land |= province->get_controller() == &country;
		}
	}
	if (!holds_enemy_land || would_accept_peace(instance_manager, war, attacker_side, peace_type_t::WHITE_PEACE)) {
		return {};
	}
	return "Противник не отпускает: страна держит его земли, а его сторона побеждает";
}

bool WarManager::separate_peace(InstanceManager& instance_manager, const std::uint32_t war_id, CountryInstance& country) {
	War* const war = find_war(war_id);
	if (war == nullptr || !get_separate_peace_block_reason(instance_manager, *war, country).empty()) {
		return false;
	}
	const bool attacker_side = war->is_attacker(country);
	memory::vector<CountryInstance*>& side = attacker_side ? war->attackers : war->defenders;
	std::erase(side, &country);
	const Date today = instance_manager.get_today();
	for (CountryInstance* enemy : war->get_side(!attacker_side)) {
		truces.push_back({ &country, enemy, today + Timespan::from_days(get_war_defines().truce_days) });
		if (find_war_between(country, *enemy) == nullptr) {
			country.set_at_war_with(*enemy, false);
		}
	}
	instance_manager.get_notification_log().add(today, &country, "separate_peace", "", war->get_side(!attacker_side).front());
	SPDLOG_INFO("War {}: {} makes a separate peace", war_id, country.get_identifier());
	restore_after_peace(instance_manager);
	return true;
}

void WarManager::update_raids(InstanceManager& instance_manager) {
	OpenVicWarDefines const& wd = get_war_defines();
	for (ProvinceInstance& province : instance_manager.get_map_instance().get_province_instances()) {
		fixed_point_t multiplier = fixed_point_t::_1;
		CountryInstance const* const owner = province.get_owner();
		if (owner != nullptr && !province.province_definition.is_water()) {
			for (ArmyInstance const& army : province.get_armies()) {
				if (army.get_stance() == ArmyInstance::STANCE_RAID && army.get_country().is_at_war_with(*owner) && !army.is_moving()) {
					multiplier = wd.stance_raid_output.fixed();
					break;
				}
			}
		}
		province.set_raid_output_multiplier(multiplier);
	}
}

void WarManager::update_navy_range(InstanceManager& instance_manager) {
	OpenVicWarDefines const& wd = get_war_defines();
	MapInstance& map_instance = instance_manager.get_map_instance();
	ModifierEffectCache const& cache = instance_manager.definition_manager.get_modifier_manager().get_modifier_effect_cache();
	BuildingType const* const naval_base = instance_manager.definition_manager.get_economy_manager().get_building_type_manager().get_port_building_type();
	if (wd.navy_range_steps <= 0 || naval_base == nullptr || !naval_base->province_building_index.has_value()) {
		return;
	}
	const std::size_t province_count = map_instance.get_province_instances().size();
	std::vector<std::int32_t> distance;
	for (CountryInstance& country : instance_manager.get_country_instance_manager().get_country_instances()) {
		bool at_sea = false;
		for (NavyInstance const& navy : country.get_navies()) {
			at_sea |= !navy.empty() && navy.get_location().province_definition.is_water();
		}
		if (!at_sea) {
			continue;
		}
		fixed_point_t range_mult = fixed_point_t::_1;
		if (cache.get_supply_range() != nullptr) {
			range_mult = std::max(fixed_point_t::_0, fixed_point_t::_1 + country.get_modifier_effect_value(*cache.get_supply_range()));
		}
		const std::int32_t range = (fixed_point_t(wd.navy_range_steps) * range_mult).floor<std::int32_t>();
		// The steps over the sea from the seas of the naval bases of the country and its allies (they must hold the port)
		distance.assign(province_count + 1, -1);
		std::vector<ProvinceInstance const*> queue;
		for (ProvinceInstance const& port : map_instance.get_province_instances()) {
			CountryInstance const* const controller = port.get_controller();
			ProvinceDefinition const* const sea = port.province_definition.get_port_adjacent_province();
			if (sea == nullptr || controller == nullptr || (controller != &country && !country.has_alliance_with(*controller))
				|| type_safe::get(port.get_buildings()[naval_base->province_building_index.value()].get_level()) <= 0) {
				continue;
			}
			ProvinceInstance const& sea_instance = map_instance.get_province_instance_by_definition(*sea);
			std::int32_t& d = distance[type_safe::get(sea_instance.index)];
			if (d < 0) {
				d = 0;
				queue.push_back(&sea_instance);
			}
		}
		for (std::size_t head = 0; head < queue.size(); ++head) {
			ProvinceInstance const& current = *queue[head];
			const std::int32_t next_distance = distance[type_safe::get(current.index)] + 1;
			if (next_distance > range) {
				continue;
			}
			for (ProvinceDefinition::adjacency_t const& adjacency : current.province_definition.get_adjacencies()) {
				if (!adjacency.get_to().is_water() || adjacency.get_type() == ProvinceDefinition::adjacency_t::type_t::IMPASSABLE) {
					continue;
				}
				ProvinceInstance const& next = map_instance.get_province_instance_by_definition(adjacency.get_to());
				std::int32_t& d = distance[type_safe::get(next.index)];
				if (d < 0) {
					d = next_distance;
					queue.push_back(&next);
				}
			}
		}
		for (NavyInstance& navy : country.get_navies()) {
			ProvinceInstance const& location = navy.get_location();
			if (navy.empty() || !location.province_definition.is_water() || distance[type_safe::get(location.index)] >= 0) {
				continue;
			}
			navy.set_out_of_range(true);
			for (ShipInstance& ship : navy.get_ship_instances()) {
				const fixed_point_t floor = wd.navy_out_of_range_floor.of(ship.get_max_strength());
				const fixed_point_t loss = ship.get_strength() > floor
					? std::min(wd.navy_out_of_range_attrition.of(ship.get_max_strength()), ship.get_strength() - floor) : fixed_point_t::_0;
				ship.take_damage(std::min(ship.get_organisation(), wd.navy_out_of_range_org_loss.of(ship.get_max_organisation())), loss);
			}
		}
	}
}

void WarManager::update_war_exhaustion(InstanceManager& instance_manager) {
	OpenVicWarDefines const& wd = get_war_defines();
	ModifierEffectCache const& cache = instance_manager.definition_manager.get_modifier_manager().get_modifier_effect_cache();
	UnitInstanceManager const& unit_manager = instance_manager.get_unit_instance_manager();
	const bool month_start = instance_manager.get_today().is_month_start();
	for (CountryInstance& country : instance_manager.get_country_instance_manager().get_country_instances()) {
		if (!country.exists() || country.is_rebel_country()) {
			country.war_losses_today = 0;
			country.war_prestige_lost_today = fixed_point_t::_0;
			continue;
		}
		fixed_point_t delta = fixed_point_t::_0;
		if (country.is_at_war()) {
			// The dead of the day for the population of the country (the technologies with war_exhaustion make it smaller), the prestige lost
			fixed_point_t battles = fixed_point_t::_1;
			if (cache.get_war_exhaustion_from_battles() != nullptr) {
				battles = std::max(fixed_point_t::_0, fixed_point_t::_1 + country.get_modifier_effect_value(*cache.get_war_exhaustion_from_battles()));
			}
			if (country.war_losses_today > 0) {
				const std::int64_t population = std::max<std::int64_t>(1, static_cast<std::int64_t>(type_safe::get(country.get_total_population())));
				// In millionths, so that the dead of a day of a country of millions are not lost
				const fixed_point_t lost_share = fixed_point_t(static_cast<int32_t>(std::min<std::int64_t>(country.war_losses_today * 1000000 / population, 1000000)))
					/ 1000000;
				delta += wd.war_exhaustion_per_population_loss * lost_share * battles;
			}
			delta += wd.war_exhaustion_per_prestige * country.war_prestige_lost_today * battles;
			// The population under the enemy and the blockade
			std::int64_t population = 0, occupied = 0;
			for (ProvinceInstance const* province : country.get_owned_provinces()) {
				const std::int64_t size = static_cast<std::int64_t>(type_safe::get(province->get_total_population()));
				population += size;
				if (province->get_controller() != &country) {
					occupied += size;
				}
			}
			if (occupied > 0 && population > 0) {
				delta += wd.war_exhaustion_occupation_per_day
					* (fixed_point_t(static_cast<int32_t>(occupied * 10000 / population)) / 10000);
			}
			delta += wd.war_exhaustion_blockade_per_day * get_blockade_share(country);
		}
		if (month_start && cache.get_war_exhaustion_monthly() != nullptr) {
			delta += country.get_modifier_effect_value(*cache.get_war_exhaustion_monthly());
		}
		if (delta != fixed_point_t::_0) {
			country.change_war_exhaustion(delta);
		}
		// The early years of the game: the war exhaustion stays below WAR_EXHAUSTION_EARLY_MAX
		if (instance_manager.get_today().get_year() < wd.war_exhaustion_early_until_year && country.get_war_exhaustion() > wd.war_exhaustion_early_max) {
			country.change_war_exhaustion(wd.war_exhaustion_early_max - country.get_war_exhaustion());
		}
		country.war_losses_today = 0;
		country.war_prestige_lost_today = fixed_point_t::_0;
	}
}

void WarManager::update_manpower(InstanceManager& instance_manager) {
	UnitInstanceManager& unit_manager = instance_manager.get_unit_instance_manager();
	OpenVicWarDefines const& wd = get_war_defines();
	for (CountryInstance& country : instance_manager.get_country_instance_manager().get_country_instances()) {
		// The mobilisation: the output of the economy while the men are away, the demobilisation after the war (docs/RECRUITMENT.md)
		if (country.is_mobilised()) {
			unit_manager.update_mobilisation(country);
		}
		if (country.is_rebel_country() || country.get_armies().empty()) {
			country.manpower_combat_multiplier = fixed_point_t::_1;
			country.over_manpower_notified = false;
			country.start_manpower_fitted = true;
			continue;
		}
		Manpower manpower = unit_manager.get_manpower(country);
		// The armies of the start that the pops of soldiers cannot man start weaker, at the maximum (not a loss: no one dies)
		if (!country.start_manpower_fitted) {
			country.start_manpower_fitted = true;
			if (wd.start_manpower_fit != 0 && manpower.army_men > manpower.max_men && manpower.army_men > 0) {
				const fixed_point_t share = fixed_point_t(static_cast<int32_t>(manpower.max_men * 10000 / manpower.army_men)) / 10000;
				for (ArmyInstance& army : country.get_armies()) {
					for (RegimentInstance& regiment : army.get_regiment_instances()) {
						if (regiment.is_mobilised()) {
							continue;
						}
						const fixed_point_t losses = regiment.get_unapplied_losses();
						regiment.take_damage(fixed_point_t::_0, regiment.get_strength() - regiment.get_strength() * share);
						regiment.set_unapplied_losses(losses);
					}
				}
				manpower = unit_manager.get_manpower(country);
			}
		}
		country.manpower_combat_multiplier = manpower.combat_multiplier;
		if (manpower.combat_multiplier < fixed_point_t::_1) {
			if (!country.over_manpower_notified) {
				country.over_manpower_notified = true;
				instance_manager.get_notification_log().add(
					instance_manager.get_today(), &country, "over_manpower",
					fmt::format("{}", (manpower.over_share * 100).floor<std::int32_t>())
				);
			}
		} else {
			country.over_manpower_notified = false;
		}
	}
}

void WarManager::tick(InstanceManager& instance_manager) {
	fought_today.clear();
	update_manpower(instance_manager);
	// The ways by sea of the armies (P4d): the ones in their boarding port sail, the ones at sea land
	instance_manager.get_unit_instance_manager().update_sea_transfers(instance_manager.get_map_instance());

	{
		OV_PROFILE_SCOPE(instance_manager.get_tick_profiler(), "land battles");
		update_combat(instance_manager);
	}

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
	// The armies that follow others, the raids of the day (docs/WAR_ASSUMPTIONS.md, section 10)
	instance_manager.get_unit_instance_manager().update_follows(instance_manager.get_map_instance());
	update_raids(instance_manager);
	// The fleets far from their bases (the flag is set again every day)
	for (CountryInstance& country : instance_manager.get_country_instance_manager().get_country_instances()) {
		for (NavyInstance& navy : country.get_navies()) {
			navy.set_out_of_range(false);
		}
	}
	update_navy_range(instance_manager);
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
	// The men lost in the battles and by the attrition die: they are taken off the pops (the new men of today come out of what is left)
	instance_manager.get_unit_instance_manager().apply_all_losses();
	update_army_recovery(instance_manager);
	update_attrition(instance_manager);
	instance_manager.get_unit_instance_manager().apply_all_losses();
	update_war_exhaustion(instance_manager);
	update_justifications(instance_manager);
	update_leaders(instance_manager);
	update_ai_peace(instance_manager);
	{
		OV_PROFILE_SCOPE(instance_manager.get_tick_profiler(), "plans of the AI (every 10 days)");
		update_auto_plan(instance_manager);
	}
	{
		OV_PROFILE_SCOPE(instance_manager.get_tick_profiler(), "fronts and plans");
		update_fronts(instance_manager);
	}
}

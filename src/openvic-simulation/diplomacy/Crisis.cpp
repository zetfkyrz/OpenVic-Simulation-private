#include "Crisis.hpp"

#include <algorithm>

#include "openvic-simulation/DefinitionManager.hpp"
#include "openvic-simulation/InstanceManager.hpp"
#include "openvic-simulation/ai/DiplomacyAI.hpp"
#include "openvic-simulation/country/CountryInstance.hpp"
#include "openvic-simulation/defines/DiplomacyDefines.hpp"
#include "openvic-simulation/defines/OpenVicDefines.hpp"
#include "openvic-simulation/diplomacy/CountryRelation.hpp"
#include "openvic-simulation/map/MapInstance.hpp"
#include "openvic-simulation/map/ProvinceInstance.hpp"
#include "openvic-simulation/map/State.hpp"
#include "openvic-simulation/military/War.hpp"
#include "openvic-simulation/misc/NotificationLog.hpp"
#include "openvic-simulation/population/Culture.hpp"
#include "openvic-simulation/utility/Logger.hpp"

using namespace OpenVic;

namespace {
	OpenVicDiplomacyDefines const& defines_of(InstanceManager const& instance_manager) {
		return instance_manager.definition_manager.get_define_manager().get_openvic_defines().diplomacy;
	}
	DiplomacyDefines const& vic2_of(InstanceManager const& instance_manager) {
		return instance_manager.definition_manager.get_define_manager().get_diplomacy_defines();
	}

	bool contains(memory::vector<CountryInstance*> const& list, CountryInstance const* country) {
		return std::find(list.begin(), list.end(), country) != list.end();
	}

	void erase(memory::vector<CountryInstance*>& list, CountryInstance const* country) {
		list.erase(std::remove(list.begin(), list.end(), country), list.end());
	}

	// A deterministic dice in [0, 100) from a number, the date and a salt
	int dice(const std::uint64_t key, const Date date, const std::uint64_t salt) {
		std::uint64_t x = (key + 1) * 0x9E3779B97F4A7C15ULL;
		x ^= static_cast<std::uint64_t>((date - Date {}).to_int()) * 0xBF58476D1CE4E5B9ULL;
		x ^= salt * 0x94D049BB133111EBULL;
		x ^= x >> 29;
		x *= 0xBF58476D1CE4E5B9ULL;
		x ^= x >> 32;
		return static_cast<int>(x % 100);
	}

	// The years since the start of the game of Vic2 (the factors of the prestige of a crisis grow with them)
	std::int32_t years_since_1836(const Date today) {
		return std::max(0, static_cast<std::int32_t>(today.get_year()) - 1836);
	}

	// The great powers that may take a side: not at war and not too tired
	bool may_be_interested(InstanceManager const& instance_manager, CountryInstance const& country) {
		return country.exists() && country.is_great_power() && !country.is_at_war()
			&& country.get_war_exhaustion() <= vic2_of(instance_manager).get_crisis_interest_war_exhaustion_limit();
	}

	fixed_point_t side_power(memory::vector<CountryInstance*> const& side, CountryInstance* extra) {
		fixed_point_t power = fixed_point_t::_0;
		for (CountryInstance* country : side) {
			power += country->military_power.get_untracked();
		}
		if (extra != nullptr && !contains(side, extra)) {
			power += extra->military_power.get_untracked();
		}
		return power;
	}
}

std::optional<CrisisManager::flashpoint_t> CrisisManager::get_flashpoint(State& state) {
	CountryInstance* const owner = state.get_owner();
	if (owner == nullptr || !owner->exists() || owner->is_rebel_country()) {
		return std::nullopt;
	}
	std::optional<flashpoint_t> liberation;
	for (ProvinceInstance& province : state.get_provinces()) {
		for (CountryInstance* core : province.get_cores()) {
			if (core == nullptr || core == owner || core->is_rebel_country()) {
				continue;
			}
			// (a formed union is its country)
			CountryInstance* const holder = core->union_holder != nullptr ? core->union_holder : core;
			if (holder == owner || holder->overlord == owner || owner->overlord == holder) {
				continue;
			}
			if (holder->exists()) {
				return flashpoint_t { &state, holder, false };
			}
			// a nation whose people the owner does not accept
			Culture const* const culture = holder->get_primary_culture();
			if (!liberation.has_value() && culture != nullptr && !owner->is_primary_or_accepted_culture(*culture)) {
				liberation = flashpoint_t { &state, holder, true };
			}
		}
	}
	return liberation;
}

memory::vector<CrisisManager::flashpoint_t> CrisisManager::get_hottest(InstanceManager& instance_manager, const std::size_t count) const {
	memory::vector<flashpoint_t> result;
	for (ProvinceInstance& province : instance_manager.get_map_instance().get_province_instances()) {
		State* const state = province.get_state();
		if (state == nullptr || state->get_provinces().empty() || &state->get_provinces().front().get() != &province
			|| state->get_crisis_tension() <= fixed_point_t::_0) {
			continue;
		}
		if (std::optional<flashpoint_t> flashpoint = get_flashpoint(*state); flashpoint.has_value()) {
			result.push_back(*flashpoint);
		}
	}
	std::stable_sort(result.begin(), result.end(), [](flashpoint_t const& a, flashpoint_t const& b) -> bool {
		return a.state->get_crisis_tension() > b.state->get_crisis_tension();
	});
	if (result.size() > count) {
		result.resize(count);
	}
	return result;
}

void CrisisManager::monthly_update(InstanceManager& instance_manager) {
	OpenVicDiplomacyDefines const& defines = defines_of(instance_manager);
	DiplomacyDefines const& vic2 = vic2_of(instance_manager);
	const Date today = instance_manager.get_today();
	const std::array<fixed_point_t, 8> rank_decay {
		vic2.get_rank_1_tension_decay(), vic2.get_rank_2_tension_decay(), vic2.get_rank_3_tension_decay(), vic2.get_rank_4_tension_decay(),
		vic2.get_rank_5_tension_decay(), vic2.get_rank_6_tension_decay(), vic2.get_rank_7_tension_decay(), vic2.get_rank_8_tension_decay()
	};
	State* hottest = nullptr;
	flashpoint_t hottest_flashpoint;
	// the states in the order of their first province
	for (ProvinceInstance& province : instance_manager.get_map_instance().get_province_instances()) {
		State* const state = province.get_state();
		if (state == nullptr || state->get_provinces().empty() || &state->get_provinces().front().get() != &province) {
			continue;
		}
		const std::optional<flashpoint_t> flashpoint = get_flashpoint(*state);
		fixed_point_t tension = state->get_crisis_tension();
		if (!flashpoint.has_value() || (crisis.active && crisis.state == state)) {
			tension -= defines.crisis_tension_calm_decay;
		} else if (crisis.active) {
			// (another crisis holds the world: TENSION_WHILE_CRISIS of Vic2)
			tension -= defines.crisis_tension_while_crisis;
		} else {
			CountryInstance const& owner = *state->get_owner();
			fixed_point_t gain = fixed_point_t::_0;
			if (!owner.is_at_war()) {
				gain = fixed_point_t(defines.crisis_tension_base) / 10;
				// the people of the claimant in the state
				if (Culture const* const culture = flashpoint->claimant->get_primary_culture(); culture != nullptr) {
					const std::int64_t total = static_cast<std::int64_t>(type_safe::get(state->get_total_population()));
					if (total > 0) {
						const std::int64_t people = static_cast<std::int64_t>(type_safe::get(state->get_population_by_culture(*culture)));
						const std::int32_t permille = static_cast<std::int32_t>(std::min<std::int64_t>(1000, people * 1000 / total));
						gain += fixed_point_t(defines.crisis_tension_culture) / 10 * permille / 1000;
					}
				}
				// a casus belli of the claimant against the owner
				if (!flashpoint->liberation && flashpoint->claimant->has_casus_belli_against(owner, today)) {
					gain += fixed_point_t(defines.crisis_tension_casus_belli) / 10;
				}
			}
			// the land of a great power heats slower (the decay of Vic2 is a day: x 30 a month)
			const std::size_t rank = owner.get_total_rank();
			if (rank >= 1 && rank <= rank_decay.size()) {
				gain += rank_decay[rank - 1] * 30;
			}
			tension += gain;
		}
		tension = std::clamp(tension, fixed_point_t::_0, fixed_point_t(100));
		state->set_crisis_tension(tension);
		if (flashpoint.has_value() && tension >= fixed_point_t(100) && (hottest == nullptr || tension > hottest->get_crisis_tension())) {
			hottest = state;
			hottest_flashpoint = *flashpoint;
		}
	}
	// a new crisis
	if (!crisis.active && hottest != nullptr && (!any_crisis_yet || today >= last_end + vic2.get_crisis_cooldown_duration())
		&& dice(type_safe::get(hottest->get_provinces().front().get().index), today, 0xc415) < vic2.get_crisis_base_chance().floor<std::int32_t>()) {
		start(instance_manager, hottest_flashpoint);
	}
}

bool CrisisManager::force_start(InstanceManager& instance_manager, State& state) {
	const std::optional<flashpoint_t> flashpoint = get_flashpoint(state);
	if (crisis.active || !flashpoint.has_value()) {
		return false;
	}
	start(instance_manager, *flashpoint);
	return crisis.active;
}

void CrisisManager::start(InstanceManager& instance_manager, flashpoint_t const& flashpoint) {
	crisis = {};
	crisis.active = true;
	crisis.state = flashpoint.state;
	crisis.owner = flashpoint.state->get_owner();
	crisis.claimant = flashpoint.claimant;
	crisis.liberation = flashpoint.liberation;
	crisis.start = instance_manager.get_today();
	any_crisis_yet = true;
	flashpoint.state->set_crisis_here(true);
	// the great powers of the sides lead them, the others are interested
	if (crisis.owner->is_great_power()) {
		crisis.defender_leader = crisis.owner;
		crisis.defenders.push_back(crisis.owner);
	}
	if (!crisis.liberation && crisis.claimant->is_great_power()) {
		crisis.attacker_leader = crisis.claimant;
		crisis.attackers.push_back(crisis.claimant);
	}
	for (CountryInstance& country : instance_manager.get_country_instance_manager().get_country_instances()) {
		if (&country != crisis.owner && &country != crisis.claimant && may_be_interested(instance_manager, country)) {
			crisis.interested.push_back(&country);
		}
	}
	crisis.interested_at_start = static_cast<std::uint32_t>(crisis.interested.size());
	SPDLOG_INFO("Crisis: {} {} the land of {} ({} interested)", crisis.claimant->get_identifier(), crisis.liberation ? "wants to be free in" : "claims",
		crisis.owner->get_identifier(), crisis.interested.size());
	instance_manager.get_notification_log().add(instance_manager.get_today(), crisis.owner, "crisis_started",
		crisis.liberation ? "liberation" : "claim", crisis.claimant);
}

void CrisisManager::join(CountryInstance& country, const bool attackers_side) {
	erase(crisis.interested, &country);
	memory::vector<CountryInstance*>& side = attackers_side ? crisis.attackers : crisis.defenders;
	if (!contains(side, &country)) {
		side.push_back(&country);
	}
	CountryInstance*& leader = attackers_side ? crisis.attacker_leader : crisis.defender_leader;
	if (leader == nullptr) {
		leader = &country;
	}
}

std::string_view CrisisManager::get_back_block_reason(CountryInstance const& country, const bool attackers_side) const {
	if (!crisis.active) {
		return "Кризиса нет";
	}
	if (&country == crisis.owner || &country == crisis.claimant) {
		return "Мы — сторона кризиса";
	}
	if (contains(crisis.attackers, &country) || contains(crisis.defenders, &country)) {
		return "Мы уже выбрали сторону";
	}
	if (!contains(crisis.interested, &country)) {
		return "В кризисе участвуют только великие державы, не воюющие и не уставшие от войны";
	}
	return {};
}

bool CrisisManager::back(InstanceManager& instance_manager, CountryInstance& country, const bool attackers_side) {
	if (!get_back_block_reason(country, attackers_side).empty()) {
		return false;
	}
	join(country, attackers_side);
	SPDLOG_INFO("Crisis: {} backs the {}", country.get_identifier(), attackers_side ? "claim" : "owner");
	instance_manager.get_notification_log().add(instance_manager.get_today(), &country, "crisis_backed",
		attackers_side ? "attackers" : "defenders", attackers_side ? crisis.claimant : crisis.owner);
	return true;
}

bool CrisisManager::concede(InstanceManager& instance_manager, CountryInstance& country) {
	if (!crisis.active) {
		return false;
	}
	if (&country == crisis.attacker_leader) {
		finish(instance_manager, false);
		return true;
	}
	if (&country == crisis.defender_leader) {
		finish(instance_manager, true);
		return true;
	}
	return false;
}

fixed_point_t CrisisManager::get_daily_temperature(InstanceManager const& instance_manager) const {
	if (!crisis.active || crisis.attacker_leader == nullptr || crisis.defender_leader == nullptr) {
		return fixed_point_t::_0;
	}
	DiplomacyDefines const& vic2 = vic2_of(instance_manager);
	// (the share of the interested that took a side: linear up to CRISIS_TEMPERATURE_PARTICIPANT_FACTOR times faster)
	const std::int32_t sided = static_cast<std::int32_t>(crisis.interested_at_start) - static_cast<std::int32_t>(crisis.interested.size());
	fixed_point_t factor = fixed_point_t::_1;
	if (crisis.interested_at_start > 0 && sided > 0) {
		factor += (vic2.get_crisis_temperature_participant_factor() - fixed_point_t::_1) * sided / static_cast<std::int32_t>(crisis.interested_at_start);
	}
	return vic2.get_crisis_temperature_increase() * defines_of(instance_manager).crisis_temperature_pct / 100 * factor;
}

int CrisisManager::ai_choice(InstanceManager& instance_manager, CountryInstance const& power) const {
	OpenVicDiplomacyDefines const& defines = defines_of(instance_manager);
	CountryInstance const& owner = *crisis.owner;
	CountryInstance const& claimant = *crisis.claimant;
	const auto friend_of = [&power, &defines](CountryInstance const& other) -> bool {
		return other.exists() && (power.has_alliance_with(other) || other.overlord == &power || other.get_sphere_owner_untracked() == &power
			|| power.shares_sphere_with(other) || power.get_relations_with(other) >= defines.crisis_ai_back_relation);
	};
	const bool defend = friend_of(owner);
	bool attack = !crisis.liberation && friend_of(claimant);
	// a nation of the culture group of the great power, or an owner it hates
	if (crisis.liberation) {
		Culture const* const nation_culture = claimant.get_primary_culture();
		Culture const* const own_culture = power.get_primary_culture();
		attack = (nation_culture != nullptr && own_culture != nullptr && &nation_culture->group == &own_culture->group)
			|| power.get_relations_with(owner) <= defines.crisis_ai_oppose_relation;
	}
	if (defend) {
		return -1;
	}
	if (attack) {
		return 1;
	}
	// against a rival that leads a side
	if (crisis.attacker_leader != nullptr && power.get_relations_with(*crisis.attacker_leader) <= defines.crisis_ai_oppose_relation) {
		return -1;
	}
	if (crisis.defender_leader != nullptr && power.get_relations_with(*crisis.defender_leader) <= defines.crisis_ai_oppose_relation) {
		return 1;
	}
	// a great power interested in the land (a neighbour, a navy near it) takes the side of the one it likes clearly more
	if (DiplomacyAI::interest(instance_manager, power, owner) > fixed_point_t::_0) {
		const std::int32_t owner_relation = power.get_relations_with(owner);
		const std::int32_t claimant_relation = crisis.liberation ? 0 : power.get_relations_with(claimant);
		if (claimant_relation - owner_relation >= defines.crisis_ai_prefer_relation) {
			return 1;
		}
		if (owner_relation - claimant_relation >= defines.crisis_ai_prefer_relation) {
			return -1;
		}
	}
	return 0;
}

void CrisisManager::daily_update(InstanceManager& instance_manager) {
	if (!crisis.active) {
		return;
	}
	OpenVicDiplomacyDefines const& defines = defines_of(instance_manager);
	DiplomacyDefines const& vic2 = vic2_of(instance_manager);
	const Date today = instance_manager.get_today();
	// the crisis is gone with its land or its claimant
	if (crisis.state->get_owner() != crisis.owner || !crisis.owner->exists() || (!crisis.liberation && !crisis.claimant->exists())) {
		fizzle(instance_manager);
		return;
	}
	if (crisis.liberation && crisis.claimant->exists()) {
		crisis.liberation = false;
	}
	// the interested that went to war or are gone leave the list
	std::erase_if(crisis.interested, [&instance_manager](CountryInstance* country) -> bool {
		return !may_be_interested(instance_manager, *country);
	});
	// the AI great powers choose
	for (CountryInstance* power : memory::vector<CountryInstance*> { crisis.interested }) {
		if (!power->is_ai() || contains(crisis.ai_decided, power)) {
			continue;
		}
		crisis.ai_decided.push_back(power);
		const int choice = ai_choice(instance_manager, *power);
		if (choice != 0) {
			back(instance_manager, *power, choice > 0);
		}
	}
	const std::int32_t days = (today - crisis.start).to_int();
	// a side without a leader after the days of the backers loses
	if (days >= defines.crisis_backer_days) {
		if (crisis.attacker_leader == nullptr) {
			fizzle(instance_manager);
			return;
		}
		if (crisis.defender_leader == nullptr) {
			finish(instance_manager, true);
			return;
		}
	}
	if (crisis.attacker_leader == nullptr || crisis.defender_leader == nullptr) {
		return;
	}
	// the temperature
	crisis.temperature += get_daily_temperature(instance_manager);
	for (memory::vector<CountryInstance*> const* side : { &crisis.attackers, &crisis.defenders }) {
		for (CountryInstance* member : *side) {
			if (member->is_mobilised() && !contains(crisis.mobilised, member)) {
				crisis.mobilised.push_back(member);
				crisis.temperature += vic2.get_crisis_temperature_on_mobilize();
			}
		}
	}
	if (crisis.temperature >= fixed_point_t(100)) {
		crisis.temperature = fixed_point_t(100);
		great_war(instance_manager);
		return;
	}
	// a weaker AI leader gives in sometimes (once a week; more often when the crisis is hot)
	if (today.get_day() % 7 == 1) {
		const fixed_point_t attack = side_power(crisis.attackers, crisis.liberation ? nullptr : crisis.claimant);
		const fixed_point_t defence = side_power(crisis.defenders, crisis.owner);
		const std::int32_t chance = crisis.temperature >= fixed_point_t(defines.crisis_ai_hot_temperature)
			? defines.crisis_ai_concede_chance * 2 : defines.crisis_ai_concede_chance;
		CountryInstance* const weaker = attack * 100 < defence * defines.crisis_ai_concede_power_pct ? crisis.attacker_leader
			: defence * 100 < attack * defines.crisis_ai_concede_power_pct ? crisis.defender_leader : nullptr;
		if (weaker != nullptr && weaker->is_ai() && dice(type_safe::get(weaker->index), today, 0xc416) < chance) {
			SPDLOG_INFO("Crisis: {} gives in", weaker->get_identifier());
			concede(instance_manager, *weaker);
		}
	}
}

void CrisisManager::finish(InstanceManager& instance_manager, const bool attackers_win) {
	DiplomacyDefines const& vic2 = vic2_of(instance_manager);
	const Date today = instance_manager.get_today();
	const std::int32_t years = years_since_1836(today);
	CountryRelationManager& relations = instance_manager.get_country_relation_manager();
	SPDLOG_INFO("Crisis: the {} win", attackers_win ? "attackers" : "defenders");
	if (attackers_win) {
		// the state goes to the claimant (a liberated nation gets its country)
		CountryInstance* const claimant = crisis.claimant;
		for (ProvinceInstance& province : crisis.state->get_provinces()) {
			province.set_owner(claimant);
			province.set_controller(claimant);
		}
		if (claimant->get_capital() == nullptr || claimant->get_capital()->get_owner() != claimant) {
			claimant->set_capital(claimant->get_owned_provinces().empty() ? nullptr : *claimant->get_owned_provinces().begin());
		}
		if (crisis.owner->get_capital() == nullptr || crisis.owner->get_capital()->get_owner() != crisis.owner) {
			crisis.owner->set_capital(crisis.owner->get_owned_provinces().empty() ? nullptr : *crisis.owner->get_owned_provinces().begin());
		}
	}
	memory::vector<CountryInstance*> const& winners = attackers_win ? crisis.attackers : crisis.defenders;
	memory::vector<CountryInstance*> const& losers = attackers_win ? crisis.defenders : crisis.attackers;
	CountryInstance* const winner_leader = attackers_win ? crisis.attacker_leader : crisis.defender_leader;
	const fixed_point_t prestige = vic2.get_crisis_winner_prestige_factor_base() + vic2.get_crisis_winner_prestige_factor_year() * years;
	for (CountryInstance* winner : winners) {
		winner->add_prestige(prestige);
	}
	if (winner_leader != nullptr) {
		for (CountryInstance* loser : losers) {
			if (loser != winner_leader) {
				relations.change_relation(loser, winner_leader, -vic2.get_crisis_winner_relations_impact().round<std::int32_t>());
			}
		}
	}
	// the interested that stayed out lose prestige (a share of theirs)
	const fixed_point_t factor = vic2.get_crisis_did_not_take_side_prestige_factor_base() + vic2.get_crisis_did_not_take_side_prestige_factor_year() * years;
	for (CountryInstance* country : crisis.interested) {
		country->add_prestige(country->get_prestige_untracked() * factor);
	}
	instance_manager.get_notification_log().add(today, winner_leader != nullptr ? winner_leader : crisis.claimant, "crisis_resolved",
		attackers_win ? "attackers" : "defenders", crisis.owner);
	crisis.state->set_crisis_tension(fixed_point_t::_0);
	crisis.state->set_crisis_here(false);
	crisis.active = false;
	last_end = today;
}

void CrisisManager::fizzle(InstanceManager& instance_manager) {
	SPDLOG_INFO("Crisis: fades away");
	instance_manager.get_notification_log().add(instance_manager.get_today(), crisis.owner, "crisis_fizzled", "", crisis.claimant);
	crisis.state->set_crisis_tension(fixed_point_t::_0);
	crisis.state->set_crisis_here(false);
	crisis.active = false;
	last_end = instance_manager.get_today();
}

void CrisisManager::great_war(InstanceManager& instance_manager) {
	WarManager& war_manager = instance_manager.get_war_manager();
	CountryInstance& attacker = *crisis.attacker_leader;
	CountryInstance& owner = *crisis.owner;
	ProvinceInstance* const state_province = crisis.state->get_capital() != nullptr ? crisis.state->get_capital()
		: &crisis.state->get_provinces().front().get();
	if (&attacker == &owner || !war_manager.declare_war(instance_manager, attacker, owner, WarGoal { WarGoal::type_t::CONQUER_STATE, state_province })) {
		fizzle(instance_manager);
		return;
	}
	War* const war = war_manager.find_war_between(attacker, owner);
	if (war != nullptr) {
		war->great_war = true;
		const std::uint32_t war_id = war->id;
		for (CountryInstance* member : crisis.attackers) {
			war_manager.join_war(instance_manager, war_id, *member, true);
		}
		if (!crisis.liberation && crisis.claimant != &attacker) {
			war_manager.join_war(instance_manager, war_id, *crisis.claimant, true);
		}
		for (CountryInstance* member : crisis.defenders) {
			war_manager.join_war(instance_manager, war_id, *member, false);
		}
	}
	SPDLOG_INFO("Crisis: the great war of {} against {}", attacker.get_identifier(), owner.get_identifier());
	instance_manager.get_notification_log().add(instance_manager.get_today(), &attacker, "great_war", "", &owner);
	crisis.state->set_crisis_here(false);
	crisis.active = false;
	last_end = instance_manager.get_today();
}

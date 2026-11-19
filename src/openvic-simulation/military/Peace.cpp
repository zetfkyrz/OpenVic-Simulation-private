#include "openvic-simulation/DefinitionManager.hpp"
#include "openvic-simulation/defines/DiplomacyDefines.hpp"
#include "openvic-simulation/economy/BuildingType.hpp"
#include "openvic-simulation/economy/BuildingInstance.hpp"
#include "openvic-simulation/misc/NotificationLog.hpp"
#include "openvic-simulation/diplomacy/Subjects.hpp"
#include "openvic-simulation/diplomacy/Unions.hpp"
#include "openvic-simulation/country/Sphere.hpp"
#include "War.hpp"

#include <algorithm>
#include <unordered_set>

#include "openvic-simulation/InstanceManager.hpp"
#include "openvic-simulation/country/CountryInstance.hpp"
#include "openvic-simulation/map/MapInstance.hpp"
#include "openvic-simulation/map/ProvinceInstance.hpp"
#include "openvic-simulation/map/State.hpp"
#include "openvic-simulation/utility/Logger.hpp"

using namespace OpenVic;

// Peace negotiations: terms drafted by a country, the answer of the other side and the offers made by the AI.

namespace {
	bool side_contains(memory::vector<CountryInstance*> const& countries, CountryInstance const* country) {
		return std::find(countries.begin(), countries.end(), country) != countries.end();
	}

	// One province that stands for the state (the capital of the state, or any province of it)
	ProvinceInstance const* get_state_key(ProvinceInstance const& province) {
		State const* const state = province.get_state();
		if (state == nullptr) {
			return &province;
		}
		if (state->get_capital() != nullptr) {
			return state->get_capital();
		}
		return state->get_provinces().empty() ? &province : &state->get_provinces().front().get();
	}

	bool same_state(ProvinceInstance const& first, ProvinceInstance const& second) {
		return first.get_state() != nullptr ? first.get_state() == second.get_state() : &first == &second;
	}

	std::uint64_t offer_key(const std::uint32_t war_id, CountryInstance const* country) {
		return (static_cast<std::uint64_t>(war_id) << 32) ^ static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(country) >> 4);
	}
}

bool WarManager::is_war_goal(War const& war, const bool attackers, const WarGoal::type_t type, ProvinceInstance const* const state_province) {
	const auto matches = [type, state_province](WarGoal const& goal) -> bool {
		if (goal.type != type) {
			return false;
		}
		if (type != WarGoal::type_t::CONQUER_STATE) {
			return true;
		}
		return state_province != nullptr && goal.state_province != nullptr && same_state(*goal.state_province, *state_province);
	};
	if (attackers && matches(war.goal)) {
		return true;
	}
	for (WarGoalEntry const& entry : war.extra_goals) {
		if (entry.attackers == attackers && matches(entry.goal)) {
			return true;
		}
	}
	return false;
}

memory::string WarManager::get_add_war_goal_block_reason(
	InstanceManager const& instance_manager, War const& war, CountryInstance const& country, WarGoal const& goal
) const {
	if (!war.has_participant(country)) {
		return "Мы не участвуем в этой войне";
	}
	const bool attackers = war.is_attacker(country);
	if (war.get_side(attackers).empty() || war.get_side(attackers).front() != &country) {
		return "Цели добавляет только лидер стороны";
	}
	if (goal.type == WarGoal::type_t::NONE) {
		return "Нет такой цели";
	}
	if (goal.type == WarGoal::type_t::CONQUER_STATE) {
		if (goal.state_province == nullptr || goal.state_province->get_owner() == nullptr
			|| !war.has_participant(*goal.state_province->get_owner()) || war.is_attacker(*goal.state_province->get_owner()) == attackers) {
			return "Область должна принадлежать врагу в этой войне";
		}
	}
	if (is_war_goal(war, attackers, goal.type, goal.state_province)) {
		return "Это уже цель войны";
	}
	const fixed_point_t cost = instance_manager.definition_manager.get_define_manager().get_diplomacy_defines().get_addwargoal_diplomatic_cost();
	if (country.get_diplomatic_points() < cost) {
		return "Не хватает очков дипломатии";
	}
	return {};
}

bool WarManager::add_war_goal(InstanceManager& instance_manager, const std::uint32_t war_id, CountryInstance& country, WarGoal const& goal) {
	War* const war = find_war(war_id);
	if (war == nullptr || !get_add_war_goal_block_reason(instance_manager, *war, country, goal).empty()) {
		return false;
	}
	DiplomacyDefines const& dd = instance_manager.definition_manager.get_define_manager().get_diplomacy_defines();
	const bool attackers = war->is_attacker(country);
	CountryInstance* const enemy = war->get_side(!attackers).empty() ? nullptr : war->get_side(!attackers).front();
	country.set_diplomatic_points(country.get_diplomatic_points() - dd.get_addwargoal_diplomatic_cost());
	// a goal without a casus belli against the enemy leader costs infamy
	if (enemy == nullptr || !country.has_casus_belli_against(*enemy, instance_manager.get_today())) {
		country.change_infamy(dd.get_add_unjustified_goal_badboy());
	}
	war->extra_goals.push_back({ attackers, goal });
	if (enemy != nullptr) {
		instance_manager.get_notification_log().add(instance_manager.get_today(), enemy, "war_goal_added", "", &country);
	}
	return true;
}

void WarManager::update_reparations(InstanceManager& instance_manager) {
	const Date today = instance_manager.get_today();
	if (!reparations.empty()) {
		const fixed_point_t share = instance_manager.definition_manager.get_define_manager().get_diplomacy_defines().get_reparations_tax_hit();
		std::erase_if(reparations, [today](reparation_t const& entry) -> bool {
			return today >= entry.until || !entry.payer->exists() || !entry.receiver->exists();
		});
		for (reparation_t const& entry : reparations) {
			const fixed_point_t amount = std::min(
				share * std::max(entry.payer->get_income_yesterday(), fixed_point_t::_0), entry.payer->get_cash_stockpile().load()
			);
			if (amount > fixed_point_t::_0) {
				entry.payer->spend_cash(amount);
				entry.receiver->add_cash(amount);
			}
		}
	}
	if (today.is_month_start()) {
		for (CountryInstance& country : instance_manager.get_country_instance_manager().get_country_instances()) {
			if (country.is_disarmed() && today >= country.disarmed_until) {
				country.set_disarmed(false);
			}
		}
	}
}

PeaceTerms& WarManager::get_peace_draft(CountryInstance const& country, const std::uint32_t war_id) {
	PeaceTerms& draft = peace_drafts[&country];
	if (draft.war_id != war_id) {
		draft = {};
		draft.war_id = war_id;
	}
	return draft;
}

PeaceTerms const* WarManager::find_peace_draft(CountryInstance const& country) const {
	const auto it = peace_drafts.find(&country);
	return it != peace_drafts.end() ? &it->second : nullptr;
}

void WarManager::clear_peace_draft(CountryInstance const& country) {
	peace_drafts.erase(&country);
}

PeaceResponse const* WarManager::get_last_response(CountryInstance const& country) const {
	const auto it = last_responses.find(&country);
	return it != last_responses.end() ? &it->second : nullptr;
}

fixed_point_t WarManager::get_state_cost(
	War const& war, const bool winners_are_attackers, ProvinceInstance const& state_province
) const {
	CountryInstance const* const owner = state_province.get_owner();
	if (owner == nullptr) {
		return fixed_point_t::_0;
	}

	std::int32_t total_weight = 0;
	for (ProvinceInstance const* province : owner->get_owned_provinces()) {
		total_weight += get_province_weight(*province);
	}
	if (total_weight <= 0) {
		return fixed_point_t::_0;
	}

	std::int32_t state_weight = 0;
	if (State const* const state = state_province.get_state()) {
		for (ProvinceInstance const& province : state->get_provinces()) {
			state_weight += get_province_weight(province);
		}
	} else {
		state_weight = get_province_weight(state_province);
	}

	fixed_point_t cost = fixed_point_t(state_weight) / total_weight * get_war_defines().war_score_max;
	if (cost < fixed_point_t::_1) {
		cost = fixed_point_t::_1;
	}
	// a great war (version 104): everything is cheaper
	if (war.great_war && great_war_cost_mod > fixed_point_t::_0) {
		cost *= great_war_cost_mod;
	}

	// A war goal of the winners costs half (the goal of the declaration and the goals added in the war, version 100; the states of the
	// union in a war of unification, version 103)
	if (is_war_goal(war, winners_are_attackers, WarGoal::type_t::CONQUER_STATE, &state_province)
		|| is_union_goal_state(war, winners_are_attackers, state_province)) {
		cost /= get_peace_defines().war_goal_cost_divisor;
	}
	return cost;
}

memory::vector<ProvinceInstance const*> WarManager::get_peace_candidates(
	War const& war, CountryInstance const& proposer, const bool concede
) const {
	memory::vector<ProvinceInstance const*> result;
	const bool proposer_is_attacker = war.is_attacker(proposer);
	const bool winners_are_attackers = concede ? !proposer_is_attacker : proposer_is_attacker;
	memory::vector<CountryInstance*> const& winners = war.get_side(winners_are_attackers);
	memory::vector<CountryInstance*> const& losers = war.get_side(!winners_are_attackers);

	for (CountryInstance const* loser : losers) {
		std::unordered_set<State const*> seen;
		for (ProvinceInstance const* province : loser->get_owned_provinces()) {
			State const* const state = province->get_state();
			if (state != nullptr && !seen.insert(state).second) {
				continue;
			}

			bool held_by_winners = false;
			if (state != nullptr) {
				for (ProvinceInstance const& state_province : state->get_provinces()) {
					CountryInstance const* const controller = state_province.get_controller();
					if (controller != loser && side_contains(winners, controller)) {
						held_by_winners = true;
						break;
					}
				}
			} else {
				held_by_winners = province->get_controller() != loser && side_contains(winners, province->get_controller());
			}

			const bool is_goal = (winners_are_attackers && war.goal.type == WarGoal::type_t::CONQUER_STATE
				&& war.goal.state_province != nullptr && same_state(*war.goal.state_province, *province))
				|| is_union_goal_state(war, winners_are_attackers, *province);

			if (held_by_winners || is_goal) {
				result.push_back(get_state_key(*province));
			}
		}
	}
	return result;
}

fixed_point_t WarManager::get_terms_cost(War const& war, CountryInstance const& proposer, PeaceTerms const& terms) const {
	const bool proposer_is_attacker = war.is_attacker(proposer);
	const bool winners_are_attackers = terms.concede ? !proposer_is_attacker : proposer_is_attacker;

	fixed_point_t cost = fixed_point_t::_0;
	for (ProvinceInstance const* state_province : terms.states) {
		cost += get_state_cost(war, winners_are_attackers, *state_province);
	}
	const std::int32_t divisor = std::max(1, get_peace_defines().war_goal_cost_divisor);
	if (terms.reparations) {
		const fixed_point_t reparations = get_peace_defines().reparations_cost;
		cost += is_war_goal(war, winners_are_attackers, WarGoal::type_t::REPARATIONS) ? reparations / divisor : reparations;
	}
	if (terms.humiliate) {
		const fixed_point_t humiliate = get_peace_defines().humiliate_cost;
		cost += is_war_goal(war, winners_are_attackers, WarGoal::type_t::HUMILIATE) ? humiliate / divisor : humiliate;
	}
	// version 100 (stage D5)
	if (terms.disarm) {
		cost += get_peace_defines().disarm_cost;
	}
	if (terms.destroy_forts) {
		cost += get_peace_defines().destroy_forts_cost;
	}
	if (terms.destroy_naval_bases) {
		cost += get_peace_defines().destroy_naval_bases_cost;
	}
	if (terms.remove_cores) {
		cost += get_peace_defines().remove_cores_cost;
	}
	// version 101 (stage D6)
	if (terms.make_puppet) {
		cost += get_peace_defines().puppet_cost;
	}
	if (terms.independence) {
		const fixed_point_t independence = get_peace_defines().independence_cost;
		cost += is_war_goal(war, winners_are_attackers, WarGoal::type_t::INDEPENDENCE) ? independence / divisor : independence;
	}
	// version 103 (stage D8)
	if (terms.hegemony) {
		cost += get_peace_defines().hegemony_cost;
	}
	// a great war (version 104): the terms besides the states are cheaper too (the states are in get_state_cost)
	if (war.great_war && great_war_cost_mod > fixed_point_t::_0) {
		fixed_point_t states = fixed_point_t::_0;
		for (ProvinceInstance const* state_province : terms.states) {
			states += get_state_cost(war, winners_are_attackers, *state_province);
		}
		cost = states + (cost - states) * great_war_cost_mod;
	}
	return cost;
}

bool WarManager::is_union_goal_state(War const& war, const bool winners_are_attackers, ProvinceInstance const& state_province) {
	if (war.union_country == nullptr || !winners_are_attackers || war.goal.type != WarGoal::type_t::UNIFICATION
		|| state_province.get_state() == nullptr) {
		return false;
	}
	// (not the state of the capital of its owner)
	CountryInstance const* const owner = state_province.get_owner();
	if (owner != nullptr && owner->get_capital() != nullptr && owner->get_capital()->get_state() == state_province.get_state()) {
		return false;
	}
	return Unions::is_union_state(*state_province.get_state(), *war.union_country);
}

std::string_view WarManager::get_hegemony_block_reason(
	InstanceManager& instance_manager, War const& war, CountryInstance const& proposer, PeaceTerms const& terms
) const {
	if (!terms.hegemony) {
		return {};
	}
	const bool proposer_is_attacker = war.is_attacker(proposer);
	const bool winners_are_attackers = terms.concede ? !proposer_is_attacker : proposer_is_attacker;
	if (war.get_side(winners_are_attackers).empty() || war.get_side(!winners_are_attackers).empty()) {
		return "Война без сторон";
	}
	CountryInstance const& winner = *war.get_side(winners_are_attackers).front();
	if (Unions::get_union_definition(winner) == nullptr) {
		return "У культурной группы победителя нет союза наций";
	}
	if (Unions::get_hegemony_members(instance_manager, winner, *war.get_side(!winners_are_attackers).front()).empty()) {
		return "В сфере побеждённого нет стран культурной группы победителя";
	}
	return {};
}

std::string_view WarManager::get_subject_terms_block_reason(War const& war, CountryInstance const& proposer, PeaceTerms const& terms) const {
	if (!terms.make_puppet && !terms.independence) {
		return {};
	}
	const bool proposer_is_attacker = war.is_attacker(proposer);
	const bool winners_are_attackers = terms.concede ? !proposer_is_attacker : proposer_is_attacker;
	if (war.get_side(winners_are_attackers).empty() || war.get_side(!winners_are_attackers).empty()) {
		return "Война без сторон";
	}
	CountryInstance const& winner = *war.get_side(winners_are_attackers).front();
	CountryInstance const& loser = *war.get_side(!winners_are_attackers).front();
	if (terms.make_puppet && terms.independence) {
		return "Нельзя одновременно сделать марионеткой и получить независимость";
	}
	if (terms.make_puppet) {
		if (loser.is_great_power()) {
			return "Великую державу нельзя сделать марионеткой";
		}
		if (loser.overlord != nullptr) {
			return "Побеждённый уже чей-то субъект";
		}
		if (winner.overlord != nullptr) {
			return "У субъекта не может быть своих субъектов";
		}
	}
	if (terms.independence && winner.overlord != &loser) {
		return "Независимость — только для субъекта побеждённого";
	}
	return {};
}

bool WarManager::evaluate_offer(
	InstanceManager const& instance_manager, War const& war, const bool proposer_is_attacker, PeaceTerms const& terms,
	memory::string* reason
) const {
	const auto say = [reason](std::string_view text) -> void {
		if (reason != nullptr) {
			*reason = text;
		}
	};

	CountryInstance const* const proposer = war.get_side(proposer_is_attacker).front();
	const std::int32_t days = (instance_manager.get_today() - war.start_date).to_int();
	const fixed_point_t proposer_score = war.get_score(proposer_is_attacker);
	const fixed_point_t other_score = war.get_score(!proposer_is_attacker);
	const fixed_point_t cost = get_terms_cost(war, *proposer, terms);
	OpenVicPeaceDefines const& pd = get_peace_defines();

	// The leader of the side that answers is tired of the war
	CountryInstance const* const receiver = war.get_side(!proposer_is_attacker).front();
	fixed_point_t tiredness = fixed_point_t::_0;
	if (receiver->get_war_exhaustion_max() > fixed_point_t::_0) {
		tiredness = receiver->get_war_exhaustion() / receiver->get_war_exhaustion_max();
	}

	const bool empty_terms = terms.states.empty() && !terms.reparations && !terms.humiliate && !terms.disarm && !terms.destroy_forts
		&& !terms.destroy_naval_bases && !terms.remove_cores && !terms.make_puppet && !terms.independence && !terms.hegemony;
	if (std::string_view const subject_reason = get_subject_terms_block_reason(war, *proposer, terms); !subject_reason.empty()) {
		say(subject_reason);
		return false;
	}

	if (terms.concede) {
		// The proposer is losing and gives up territory: accepted if it is enough of what the winner could take
		if (empty_terms) {
			// White peace asked by the losing side
			if (other_score - proposer_score < pd.white_peace_score_gap || days > pd.white_peace_war_days
				|| tiredness > pd.white_peace_tiredness.fixed()) {
				say("Противник согласен на белый мир");
				return true;
			}
			say("Противник побеждает и не хочет белого мира");
			return false;
		}
		if (cost * pd.concede_cost_factor >= other_score - pd.concede_score_margin || tiredness > pd.concede_tiredness.fixed()
			|| days > pd.white_peace_war_days) {
			say("Противник доволен уступками");
			return true;
		}
		say("Противник требует больше: уступки слишком малы по сравнению с его военными очками");
		return false;
	}

	if (empty_terms) {
		if (other_score - proposer_score < pd.white_peace_score_gap || days > pd.white_peace_war_days
				|| tiredness > pd.white_peace_tiredness.fixed()) {
			say("Противник согласен на белый мир");
			return true;
		}
		say("Противник побеждает: белый мир отвергнут");
		return false;
	}

	// The proposer demands: the demand has to fit what the proposer could enforce
	fixed_point_t available = proposer_score - other_score / pd.demand_other_score_divisor;
	if (proposer_score >= pd.demand_strong_score) {
		available += pd.demand_strong_bonus;
	}
	available += tiredness * pd.demand_tiredness_bonus;
	available += fixed_point_t(std::min(days / 365, pd.demand_years_cap)) * pd.demand_year_bonus;
	if (receiver->get_capital() != nullptr) {
		// The capital of the receiver is in the hands of the proposer's side
		if (side_contains(war.get_side(proposer_is_attacker), receiver->get_capital()->get_controller())
			&& receiver->get_capital()->get_controller() != receiver) {
			available += pd.demand_capital_bonus;
		}
	}

	if (cost <= available) {
		say("Противник принимает условия");
		return true;
	}
	say("Противник отвергает условия: они слишком тяжёлые для текущего положения на фронтах");
	return false;
}

void WarManager::apply_peace_terms(
	InstanceManager& instance_manager, War const& war, const bool proposer_is_attacker, PeaceTerms const& terms
) {
	const bool winners_are_attackers = terms.concede ? !proposer_is_attacker : proposer_is_attacker;
	memory::vector<CountryInstance*> const& winners = war.get_side(winners_are_attackers);
	memory::vector<CountryInstance*> const& losers = war.get_side(!winners_are_attackers);
	if (winners.empty() || losers.empty()) {
		return;
	}
	SPDLOG_INFO("Peace in war {}: {} wins against {}, {} states, attackers {:.1f} defenders {:.1f}{}", war.id, winners.front()->get_identifier(),
		losers.front()->get_identifier(), terms.states.size(), static_cast<double>(war.attacker_score), static_cast<double>(war.defender_score),
		terms.states.empty() ? " (white peace)" : "");
	if (terms.reparations || terms.disarm || terms.destroy_forts || terms.destroy_naval_bases || terms.remove_cores || terms.humiliate) {
		SPDLOG_INFO("Peace in war {}: also{}{}{}{}{}{}", war.id, terms.reparations ? " reparations" : "", terms.disarm ? " disarmament" : "",
			terms.destroy_forts ? " forts" : "", terms.destroy_naval_bases ? " naval bases" : "", terms.remove_cores ? " cores" : "",
			terms.humiliate ? " humiliation" : "");
	}
	if (terms.make_puppet || terms.independence) {
		SPDLOG_INFO("Peace in war {}:{}{}", war.id, terms.make_puppet ? " puppet" : "", terms.independence ? " independence" : "");
	}

	std::unordered_set<CountryInstance*> changed_countries;
	for (ProvinceInstance const* state_province : terms.states) {
		CountryInstance* const old_owner = const_cast<CountryInstance*>(state_province->get_owner());
		if (old_owner == nullptr || !side_contains(losers, old_owner)) {
			continue;
		}

		// The state goes to the winner that holds a province of it, the leader otherwise
		CountryInstance* recipient = nullptr;
		State* const state = const_cast<State*>(state_province->get_state());
		if (state != nullptr) {
			for (ProvinceInstance& province : state->get_provinces()) {
				CountryInstance* const controller = province.get_controller();
				if (controller != old_owner && side_contains(winners, controller)) {
					recipient = controller;
					break;
				}
			}
		}
		if (recipient == nullptr) {
			recipient = winners.front();
		}

		if (state != nullptr) {
			for (ProvinceInstance& province : state->get_provinces()) {
				province.set_owner(recipient);
				province.set_controller(recipient);
			}
		} else {
			ProvinceInstance& province = const_cast<ProvinceInstance&>(*state_province);
			province.set_owner(recipient);
			province.set_controller(recipient);
		}
		changed_countries.insert(old_owner);
	}

	CountryInstance& winner_leader = *winners.front();
	CountryInstance& loser_leader = *losers.front();

	const Date today = instance_manager.get_today();
	if (terms.reparations) {
		// As in Vic2 (version 100): REPARATIONS_TAX_HIT of the income of the leader of the losers every day for REPARATIONS_YEARS years
		reparations.push_back({ &loser_leader, &winner_leader, today + Timespan::from_years(get_peace_defines().reparations_years) });
	}
	if (terms.disarm) {
		// no recruitment and the static modifier "disarming" for DISARM_YEARS years
		loser_leader.set_disarmed(true);
		loser_leader.disarmed_until = today + Timespan::from_years(get_peace_defines().disarm_years);
	}
	if (terms.destroy_forts || terms.destroy_naval_bases) {
		for (CountryInstance* loser : losers) {
			for (ProvinceInstance* province : loser->get_owned_provinces()) {
				for (BuildingInstance& building : province->get_mutable_buildings()) {
					std::string_view const identifier = building.building_type.get_identifier();
					if ((terms.destroy_forts && identifier == "fort") || (terms.destroy_naval_bases && identifier == "naval_base")) {
						building.set_level(building_level_t { 0 });
					}
				}
			}
		}
	}
	if (terms.remove_cores) {
		// the cores of the losers on the land of the winners
		for (CountryInstance* winner : winners) {
			for (ProvinceInstance* province : winner->get_owned_provinces()) {
				for (CountryInstance* loser : losers) {
					if (province->get_cores().contains(loser)) {
						province->remove_core(*loser, false);
					}
				}
			}
		}
	}
	if (terms.humiliate) {
		winner_leader.add_prestige(get_peace_defines().humiliate_prestige);
		loser_leader.add_prestige(-get_peace_defines().humiliate_prestige);
	}
	// version 101 (stage D6): the subjects
	// version 103 (stage D8): the hegemony
	if (terms.hegemony && get_hegemony_block_reason(instance_manager, war, *(terms.concede ? losers.front() : winners.front()), terms).empty()) {
		for (CountryInstance* member : Unions::get_hegemony_members(instance_manager, winner_leader, loser_leader)) {
			instance_manager.get_sphere_manager().leave(instance_manager, *member);
		}
		winner_leader.add_prestige(instance_manager.definition_manager.get_define_manager().get_diplomacy_defines().get_prestige_clear_union_sphere_base());
		instance_manager.get_notification_log().add(today, &winner_leader, "hegemony_asserted", "", &loser_leader);
	}
	if ((terms.make_puppet || terms.independence) && get_subject_terms_block_reason(war, *(terms.concede ? losers.front() : winners.front()),
		terms).empty()) {
		if (terms.independence) {
			Subjects::release(instance_manager, winner_leader);
			instance_manager.get_notification_log().add(today, &winner_leader, "independence_won", "", &loser_leader);
		} else {
			Subjects::make_subject(instance_manager, loser_leader, winner_leader, CountryInstance::subject_type_t::PUPPET);
		}
	}

	for (CountryInstance* loser : losers) {
		loser->set_last_war_loss_date(instance_manager.get_today());
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

bool WarManager::send_peace_offer(InstanceManager& instance_manager, CountryInstance& proposer, PeaceTerms const& terms) {
	const auto respond = [this, &instance_manager, &proposer](const bool accepted, std::string_view text) -> bool {
		PeaceResponse& response = last_responses[&proposer];
		response.accepted = accepted;
		response.text = text;
		response.date = instance_manager.get_today();
		return accepted;
	};

	War const* const war = find_war(terms.war_id);
	if (war == nullptr || !war->has_participant(proposer)) {
		return respond(false, "Война не найдена");
	}
	const bool proposer_is_attacker = war->is_attacker(proposer);

	// Every state of the terms must be one of the candidates, and the cost must fit the war score
	const auto candidates = get_peace_candidates(*war, proposer, terms.concede);
	for (ProvinceInstance const* state_province : terms.states) {
		const bool allowed = std::any_of(candidates.begin(), candidates.end(), [state_province](ProvinceInstance const* candidate) -> bool {
			return same_state(*candidate, *state_province);
		});
		if (!allowed) {
			return respond(false, "Нельзя потребовать эту область: у победителя нет в ней позиций");
		}
	}

	if (std::string_view const subject_reason = get_subject_terms_block_reason(*war, proposer, terms); !subject_reason.empty()) {
		return respond(false, subject_reason);
	}
	if (std::string_view const hegemony_reason = get_hegemony_block_reason(instance_manager, *war, proposer, terms); !hegemony_reason.empty()) {
		return respond(false, hegemony_reason);
	}
	const fixed_point_t cost = get_terms_cost(*war, proposer, terms);
	const fixed_point_t available = terms.concede ? war->get_score(!proposer_is_attacker) : war->get_score(proposer_is_attacker);
	if (cost > available) {
		return respond(false, "Не хватает военных очков для таких условий");
	}

	memory::string reason;
	if (!evaluate_offer(instance_manager, *war, proposer_is_attacker, terms, &reason)) {
		return respond(false, reason);
	}

	const std::uint32_t war_id = war->id;
	apply_peace_terms(instance_manager, *war, proposer_is_attacker, terms);
	end_war(instance_manager, war_id);
	clear_peace_draft(proposer);
	return respond(true, "Мир заключён");
}

memory::vector<PeaceOffer> WarManager::get_offers_for(CountryInstance const& country) const {
	memory::vector<PeaceOffer> result;
	for (PeaceOffer const& offer : offers) {
		War const* const war = get_war_by_id(offer.war_id);
		if (war == nullptr || !war->has_participant(country)) {
			continue;
		}
		if (war->is_attacker(country) != offer.from_is_attacker) {
			result.push_back(offer);
		}
	}
	return result;
}

bool WarManager::answer_peace_offer(
	InstanceManager& instance_manager, CountryInstance& country, const std::uint32_t war_id, const bool accept
) {
	for (auto it = offers.begin(); it != offers.end(); ++it) {
		if (it->war_id != war_id) {
			continue;
		}
		War const* const war = get_war_by_id(war_id);
		if (war == nullptr || !war->has_participant(country) || war->is_attacker(country) == it->from_is_attacker) {
			continue;
		}

		const PeaceOffer offer = *it;
		offers.erase(it);
		if (!accept) {
			next_offer_date[offer_key(war_id, offer.from)] = instance_manager.get_today() + Timespan::from_days(get_peace_defines().decline_cooldown_days);
			return false;
		}

		apply_peace_terms(instance_manager, *war, offer.from_is_attacker, offer.terms);
		end_war(instance_manager, war_id);
		return true;
	}
	return false;
}

namespace {
	// Fills terms with the states that fit in the war score (cheap ones first), at most the share of it
	PeaceTerms make_greedy_terms(
		WarManager const& manager, War const& war, CountryInstance const& proposer, const bool concede, const fixed_point_t budget,
		const bool puppet_wanted = false, const bool hegemony_possible = false
	) {
		PeaceTerms terms;
		terms.war_id = war.id;
		terms.concede = concede;

		auto candidates = manager.get_peace_candidates(war, proposer, concede);
		const bool proposer_is_attacker = war.is_attacker(proposer);
		const bool winners_are_attackers = concede ? !proposer_is_attacker : proposer_is_attacker;
		std::stable_sort(candidates.begin(), candidates.end(), [&](ProvinceInstance const* lhs, ProvinceInstance const* rhs) -> bool {
			return manager.get_state_cost(war, winners_are_attackers, *lhs) < manager.get_state_cost(war, winners_are_attackers, *rhs);
		});

		fixed_point_t remaining = budget;
		// the subjects first (version 101): the independence of a subject that wins (or that its losing overlord gives), a puppet of a small
		// loser of a great power
		{
			OpenVicPeaceDefines const& pd = manager.get_peace_defines();
			CountryInstance const* const winner = war.get_side(winners_are_attackers).front();
			CountryInstance const* const loser = war.get_side(!winners_are_attackers).front();
			if (winner->overlord == loser) {
				PeaceTerms probe = terms;
				probe.independence = true;
				const fixed_point_t cost = manager.get_terms_cost(war, proposer, probe);
				if (cost <= remaining) {
					remaining -= cost;
					terms.independence = true;
				}
			}
			// the hegemony over the countries of the culture of the winner (version 103)
			if (!concede && hegemony_possible && fixed_point_t(pd.hegemony_cost) <= remaining) {
				remaining -= pd.hegemony_cost;
				terms.hegemony = true;
			}
			if (terms.independence) {
			} else if (!concede && puppet_wanted && winner->is_great_power() && !loser->is_great_power() && loser->overlord == nullptr
				&& winner->overlord == nullptr
				&& static_cast<std::int64_t>(loser->get_owned_provinces().size()) * 100
					<= static_cast<std::int64_t>(winner->get_owned_provinces().size()) * pd.ai_puppet_size_pct
				&& fixed_point_t(pd.puppet_cost) <= remaining) {
				remaining -= pd.puppet_cost;
				terms.make_puppet = true;
			}
		}
		for (ProvinceInstance const* candidate : candidates) {
			const fixed_point_t cost = manager.get_state_cost(war, winners_are_attackers, *candidate);
			if (cost <= remaining) {
				remaining -= cost;
				terms.states.push_back(candidate);
			}
		}
		// What is left of the budget of a winner (version 100): the reparations, the forts and the disarmament of the loser, in this order
		if (!concede) {
			OpenVicPeaceDefines const& pd = manager.get_peace_defines();
			const std::int32_t divisor = std::max(1, pd.war_goal_cost_divisor);
			const fixed_point_t reparations = WarManager::is_war_goal(war, winners_are_attackers, WarGoal::type_t::REPARATIONS)
				? fixed_point_t(pd.reparations_cost) / divisor : fixed_point_t(pd.reparations_cost);
			if (reparations <= remaining) {
				remaining -= reparations;
				terms.reparations = true;
			}
			if (fixed_point_t(pd.destroy_forts_cost) <= remaining) {
				remaining -= pd.destroy_forts_cost;
				terms.destroy_forts = true;
			}
			if (fixed_point_t(pd.disarm_cost) <= remaining) {
				remaining -= pd.disarm_cost;
				terms.disarm = true;
			}
		}
		return terms;
	}
}

void WarManager::update_ai_peace(InstanceManager& instance_manager) {
	const Date today = instance_manager.get_today();

	// Offers that were not answered for long are withdrawn
	offers.erase(
		std::remove_if(offers.begin(), offers.end(), [this, today](PeaceOffer const& offer) -> bool {
			return get_war_by_id(offer.war_id) == nullptr || (today - offer.date).to_int() > get_peace_defines().offer_valid_days;
		}),
		offers.end()
	);

	// Only once a month and only on the 10th, the costs of looking at every war are small anyway
	OpenVicPeaceDefines const& pd = get_peace_defines();
	if (today.get_day() != pd.ai_peace_day_of_month) {
		return;
	}

	std::vector<std::uint32_t> war_ids;
	for (War const& war : wars) {
		war_ids.push_back(war.id);
	}

	for (const std::uint32_t war_id : war_ids) {
		War const* const war = get_war_by_id(war_id);
		if (war == nullptr || war->attackers.empty() || war->defenders.empty()) {
			continue;
		}

		CountryInstance* const attacker_leader = war->attackers.front();
		CountryInstance* const defender_leader = war->defenders.front();
		const std::int32_t days = (today - war->start_date).to_int();
		const fixed_point_t attacker_score = war->attacker_score;
		const fixed_point_t defender_score = war->defender_score;

		// Who is winning clearly
		const bool attackers_lead = attacker_score >= pd.ai_lead_score && attacker_score - defender_score >= pd.ai_lead_margin;
		const bool defenders_lead = defender_score >= pd.ai_lead_score && defender_score - attacker_score >= pd.ai_lead_margin;
		const bool stalemate = days > pd.ai_stalemate_days && !attackers_lead && !defenders_lead;

		for (const bool attacker_side : { true, false }) {
			CountryInstance& leader = *(attacker_side ? attacker_leader : defender_leader);
			CountryInstance& other_leader = *(attacker_side ? defender_leader : attacker_leader);
			if (!leader.is_ai()) {
				continue;
			}
			const auto key = offer_key(war_id, &leader);
			const auto next_it = next_offer_date.find(key);
			if (next_it != next_offer_date.end() && next_it->second > today) {
				continue;
			}

			const bool winning = attacker_side ? attackers_lead : defenders_lead;
			const bool losing = attacker_side ? defenders_lead : attackers_lead;
			// A winning AI adds a state of the enemy it holds to its goals (version 100): not more than AI_MAX_ADDED_GOALS, and without a
			// casus belli only while the infamy stays below the limit
			if (winning) {
				std::size_t added = 0;
				for (WarGoalEntry const& entry : war->extra_goals) {
					added += entry.attackers == attacker_side ? 1 : 0;
				}
				if (added < 3) {
					for (ProvinceInstance const* candidate : get_peace_candidates(*war, leader, false)) {
						if (candidate->get_controller() != &leader || is_war_goal(*war, attacker_side, WarGoal::type_t::CONQUER_STATE, candidate)) {
							continue;
						}
						const bool justified = leader.has_casus_belli_against(other_leader, today);
						const fixed_point_t infamy_after = leader.get_infamy_untracked()
							+ instance_manager.definition_manager.get_define_manager().get_diplomacy_defines().get_add_unjustified_goal_badboy();
						if (justified || CountryInstance::infamy_casus_belli_limit <= fixed_point_t::_0
							|| infamy_after <= CountryInstance::infamy_casus_belli_limit) {
							add_war_goal(instance_manager, war_id, leader, WarGoal { WarGoal::type_t::CONQUER_STATE, candidate });
						}
						break;
					}
				}
			}
			PeaceTerms terms;
			bool make_offer = false;
			if (winning) {
				// (a puppet in AI_PUPPET_CHANCE_PCT percent of the wars, by the id of the war)
				const bool puppet_wanted = static_cast<std::int32_t>((war_id * 2654435761u) % 100u) < pd.ai_puppet_chance_pct;
				PeaceTerms probe;
				probe.war_id = war_id;
				probe.hegemony = true;
				const bool hegemony_possible = get_hegemony_block_reason(instance_manager, *war, leader, probe).empty();
				terms = make_greedy_terms(
					*this, *war, leader, false, pd.ai_terms_score_share.of(war->get_score(attacker_side)), puppet_wanted, hegemony_possible
				);
				make_offer = !terms.states.empty() || terms.make_puppet || terms.independence || terms.hegemony;
			} else if (losing) {
				terms = make_greedy_terms(*this, *war, leader, true, pd.ai_terms_score_share.of(war->get_score(!attacker_side)));
				make_offer = true; // even an empty list is a request for a white peace
			} else if (stalemate) {
				terms.war_id = war_id;
				make_offer = true;
			}
			if (!make_offer) {
				continue;
			}

			if (other_leader.is_ai()) {
				// The war of two AI countries ends at once if the other side agrees
				if (evaluate_offer(instance_manager, *war, attacker_side, terms)) {
					apply_peace_terms(instance_manager, *war, attacker_side, terms);
					end_war(instance_manager, war_id);
					break;
				}
				next_offer_date[key] = today + Timespan::from_days(pd.ai_retry_days);
			} else {
				// The player decides
				PeaceOffer offer;
				offer.war_id = war_id;
				offer.from = &leader;
				offer.from_is_attacker = attacker_side;
				offer.terms = terms;
				offer.date = today;
				offers.push_back(std::move(offer));
				next_offer_date[key] = today + Timespan::from_days(pd.offer_valid_days + pd.ai_player_offer_extra_days);
			}
		}
	}
}

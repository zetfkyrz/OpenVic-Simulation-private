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

	fixed_point_t cost = fixed_point_t(state_weight) / total_weight * 100;
	if (cost < fixed_point_t::_1) {
		cost = fixed_point_t::_1;
	}

	// The war goal of the attacker costs half
	if (winners_are_attackers && war.goal.type == WarGoal::type_t::CONQUER_STATE && war.goal.state_province != nullptr
		&& same_state(*war.goal.state_province, state_province)) {
		cost /= 2;
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

			const bool is_goal = winners_are_attackers && war.goal.type == WarGoal::type_t::CONQUER_STATE
				&& war.goal.state_province != nullptr && same_state(*war.goal.state_province, *province);

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
	if (terms.reparations) {
		cost += REPARATIONS_COST;
	}
	if (terms.humiliate) {
		cost += HUMILIATE_COST;
	}
	return cost;
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

	// The leader of the side that answers is tired of the war
	CountryInstance const* const receiver = war.get_side(!proposer_is_attacker).front();
	fixed_point_t tiredness = fixed_point_t::_0;
	if (receiver->get_war_exhaustion_max() > fixed_point_t::_0) {
		tiredness = receiver->get_war_exhaustion() / receiver->get_war_exhaustion_max();
	}

	const bool empty_terms = terms.states.empty() && !terms.reparations && !terms.humiliate;

	if (terms.concede) {
		// The proposer is losing and gives up territory: accepted if it is enough of what the winner could take
		if (empty_terms) {
			// White peace asked by the losing side
			if (other_score - proposer_score < 15 || days > 3 * 365 || tiredness > fixed_point_t(1) / 2) {
				say("Противник согласен на белый мир");
				return true;
			}
			say("Противник побеждает и не хочет белого мира");
			return false;
		}
		if (cost * 2 >= other_score - 5 || tiredness > fixed_point_t(7) / 10 || days > 3 * 365) {
			say("Противник доволен уступками");
			return true;
		}
		say("Противник требует больше: уступки слишком малы по сравнению с его военными очками");
		return false;
	}

	if (empty_terms) {
		if (other_score - proposer_score < 15 || days > 3 * 365 || tiredness > fixed_point_t(1) / 2) {
			say("Противник согласен на белый мир");
			return true;
		}
		say("Противник побеждает: белый мир отвергнут");
		return false;
	}

	// The proposer demands: the demand has to fit what the proposer could enforce
	fixed_point_t available = proposer_score - other_score / 2;
	if (proposer_score >= 40) {
		available += 10;
	}
	available += tiredness * 20;
	available += fixed_point_t(std::min(days / 365, 5)) * 4;
	if (receiver->get_capital() != nullptr) {
		// The capital of the receiver is in the hands of the proposer's side
		if (side_contains(war.get_side(proposer_is_attacker), receiver->get_capital()->get_controller())
			&& receiver->get_capital()->get_controller() != receiver) {
			available += 15;
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

	if (terms.reparations) {
		// A quarter of the treasury of the leader of the losers
		const fixed_point_t amount = loser_leader.get_cash_stockpile().load() * REPARATIONS_PERCENT / 100;
		if (amount > fixed_point_t::_0) {
			loser_leader.spend_cash(amount);
			winner_leader.add_cash(amount);
		}
	}
	if (terms.humiliate) {
		winner_leader.add_prestige(HUMILIATE_PRESTIGE);
		loser_leader.add_prestige(-HUMILIATE_PRESTIGE);
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
			next_offer_date[offer_key(war_id, offer.from)] = instance_manager.get_today() + Timespan::from_days(30);
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
		WarManager const& manager, War const& war, CountryInstance const& proposer, const bool concede, const fixed_point_t budget
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
		for (ProvinceInstance const* candidate : candidates) {
			const fixed_point_t cost = manager.get_state_cost(war, winners_are_attackers, *candidate);
			if (cost <= remaining) {
				remaining -= cost;
				terms.states.push_back(candidate);
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
			return get_war_by_id(offer.war_id) == nullptr || (today - offer.date).to_int() > OFFER_VALID_DAYS;
		}),
		offers.end()
	);

	// Only once a month and only on the 10th, the costs of looking at every war are small anyway
	if (today.get_day() != 10) {
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
		const bool attackers_lead = attacker_score >= 30 && attacker_score - defender_score >= 20;
		const bool defenders_lead = defender_score >= 30 && defender_score - attacker_score >= 20;
		const bool stalemate = days > 18 * 30 && !attackers_lead && !defenders_lead;

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
			PeaceTerms terms;
			bool make_offer = false;
			if (winning) {
				terms = make_greedy_terms(*this, *war, leader, false, war->get_score(attacker_side) * 7 / 10);
				make_offer = !terms.states.empty();
			} else if (losing) {
				terms = make_greedy_terms(*this, *war, leader, true, war->get_score(!attacker_side) * 7 / 10);
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
				next_offer_date[key] = today + Timespan::from_days(60);
			} else {
				// The player decides
				PeaceOffer offer;
				offer.war_id = war_id;
				offer.from = &leader;
				offer.from_is_attacker = attacker_side;
				offer.terms = terms;
				offer.date = today;
				offers.push_back(std::move(offer));
				next_offer_date[key] = today + Timespan::from_days(OFFER_VALID_DAYS + 30);
			}
		}
	}
}

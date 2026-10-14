#include "Credit.hpp"

#include <algorithm>
#include <cstdlib>

#include "openvic-simulation/DefinitionManager.hpp"
#include "openvic-simulation/InstanceManager.hpp"
#include "openvic-simulation/country/CountryInstance.hpp"
#include "openvic-simulation/economy/BuildingType.hpp"
#include "openvic-simulation/economy/production/FactoryInstance.hpp"
#include "openvic-simulation/economy/production/ProductionType.hpp"
#include "openvic-simulation/map/MapInstance.hpp"
#include "openvic-simulation/map/State.hpp"
#include "openvic-simulation/population/Pop.hpp"
#include "openvic-simulation/population/PopManager.hpp"
#include "openvic-simulation/population/PopType.hpp"
#include "openvic-simulation/utility/Logger.hpp"

using namespace OpenVic;

namespace {
	// Money an AI country keeps for itself before it lends
	const fixed_point_t AI_LENDING_RESERVE = fixed_point_t(1000);
	// Largest share of its money an AI country lends in one loan, and the largest loan
	const fixed_point_t AI_LENDING_SHARE = fixed_point_t(3) / 10;
	const fixed_point_t AI_MAX_LOAN = fixed_point_t(25000);
	// An AI country needs a loan when its treasury is below this
	const fixed_point_t AI_NEEDS_MONEY_BELOW = fixed_point_t(3000);
	// Highest yearly rate an AI country accepts
	const fixed_point_t AI_MAX_ACCEPTED_RATE = fixed_point_t(12) / 100;
	// Smallest loan that is worth giving
	const fixed_point_t MIN_LOAN = fixed_point_t(100);
	// The capitalists invest when a project pays back in at most this many years
	constexpr std::int32_t INVESTMENT_MAX_PAYBACK_YEARS = 12;

	std::int32_t month_key(Date const date) {
		return static_cast<std::int32_t>(date.get_year()) * 12 + static_cast<std::int32_t>(date.get_month());
	}

	bool is_valid_country(CountryInstance const& country) {
		return country.exists() && !country.is_rebel_country();
	}

	PopType const* find_capitalists(InstanceManager const& instance_manager) {
		return instance_manager.definition_manager.get_pop_manager().get_pop_type_by_identifier("capitalists");
	}

	std::int32_t count_capitalists(InstanceManager const& instance_manager, State const& state) {
		PopType const* const type = find_capitalists(instance_manager);
		if (type == nullptr) {
			return 0;
		}
		std::int32_t total = 0;
		for (Pop const& pop : state.get_pops_cache_by_type()[type->index]) {
			total += type_safe::get(pop.get_size());
		}
		return total;
	}

	void change_relations(CountryInstance& first, CountryInstance& second, const std::int32_t delta) {
		const std::int32_t value = std::clamp<std::int32_t>(
			static_cast<std::int32_t>(first.get_relations_with(second)) + delta, -200, 200
		);
		first.set_relations_with(second, static_cast<CountryRelationManager::relation_value_type>(value));
	}
}

Timespan Loan::get_term_length(const term_t term) {
	switch (term) {
	case term_t::SHORT:  return Timespan::from_years(5);
	case term_t::MEDIUM: return Timespan::from_years(10);
	case term_t::LONG:   return Timespan::from_years(20);
	}
	return Timespan::from_years(5);
}

Timespan Loan::get_grace_length(const term_t term) {
	switch (term) {
	case term_t::SHORT:  return Timespan::from_years(2);
	case term_t::MEDIUM: return Timespan::from_years(4);
	case term_t::LONG:   return Timespan::from_years(8);
	}
	return Timespan::from_years(2);
}

bool Loan::can_fund(const purpose_t purpose, const std::int32_t construction_days) const {
	if (term != term_t::SHORT) {
		return true;
	}
	return purpose == purpose_t::CONSTRUCTION && construction_days > 0 && construction_days <= 365;
}

fixed_point_t CreditManager::clamp_rate(const fixed_point_t rate) {
	return std::clamp(rate, get_min_rate(), get_max_rate());
}

fixed_point_t CreditManager::get_debt_limit(CountryInstance const& country) {
	const std::int64_t population = static_cast<std::int64_t>(type_safe::get(country.get_total_population()));
	return fixed_point_t(5000) + fixed_point_t(static_cast<std::int32_t>(std::min<std::int64_t>(population / 100, 1000000000)));
}

Loan& CreditManager::add_loan(Loan&& loan) {
	loan.id = next_loan_id++;
	loans.push_back(std::make_unique<Loan>(std::move(loan)));
	return *loans.back();
}

Loan const* CreditManager::get_loan_by_id(const std::uint32_t id) const {
	for (std::unique_ptr<Loan> const& loan : loans) {
		if (loan->id == id) {
			return loan.get();
		}
	}
	return nullptr;
}

fixed_point_t CreditManager::get_total_debt(CountryInstance const& borrower) const {
	fixed_point_t total = 0;
	for (std::unique_ptr<Loan> const& loan : loans) {
		if (loan->active && loan->kind == Loan::kind_t::EXTERNAL && loan->borrower == &borrower) {
			total += loan->remaining;
		}
	}
	return total;
}

fixed_point_t CreditManager::get_monthly_interest(CountryInstance const& borrower) const {
	fixed_point_t total = 0;
	for (std::unique_ptr<Loan> const& loan : loans) {
		if (loan->active && loan->kind == Loan::kind_t::EXTERNAL && loan->borrower == &borrower) {
			total += loan->remaining * loan->annual_rate / 12;
		}
	}
	return total;
}

memory::vector<ExternalOffer> CreditManager::get_offers(
	InstanceManager const& instance_manager, CountryInstance const& borrower
) const {
	memory::vector<ExternalOffer> offers;

	const fixed_point_t debt = get_total_debt(borrower);
	const fixed_point_t limit = get_debt_limit(borrower);
	const fixed_point_t room = limit - debt;
	if (room < MIN_LOAN) {
		return offers;
	}

	// The more indebted the borrower is, the higher the rate
	const fixed_point_t debt_ratio = std::clamp(debt / limit, fixed_point_t::_0, fixed_point_t::_1);

	for (CountryInstance const& lender : instance_manager.get_country_instance_manager().get_country_instances()) {
		if (&lender == &borrower || !is_valid_country(lender) || !lender.is_ai() || lender.is_at_war_with(borrower)) {
			continue;
		}
		const fixed_point_t surplus = lender.get_spendable_cash() - AI_LENDING_RESERVE;
		if (surplus < MIN_LOAN) {
			continue;
		}

		ExternalOffer offer;
		offer.lender = const_cast<CountryInstance*>(&lender);
		offer.max_amount = std::min(std::min(surplus * AI_LENDING_SHARE, AI_MAX_LOAN), room);
		offer.rate = clamp_rate(lender.get_external_loan_rate() + debt_ratio * 5 / 100);
		if (offer.max_amount >= MIN_LOAN) {
			offers.push_back(offer);
		}
	}

	std::sort(offers.begin(), offers.end(), [](ExternalOffer const& lhs, ExternalOffer const& rhs) -> bool {
		return lhs.rate < rhs.rate;
	});
	return offers;
}

bool CreditManager::take_external_loan(
	InstanceManager& instance_manager, CountryInstance& borrower, CountryInstance& lender, fixed_point_t amount,
	const Loan::term_t term
) {
	if (&borrower == &lender || !is_valid_country(borrower) || !is_valid_country(lender)) {
		return false;
	}

	// The terms are those of the offer of the lender
	for (ExternalOffer const& offer : get_offers(instance_manager, borrower)) {
		if (offer.lender != &lender) {
			continue;
		}
		amount = std::min(amount, offer.max_amount);
		if (amount < MIN_LOAN || lender.get_spendable_cash() < amount) {
			return false;
		}

		lender.spend_cash(amount);
		borrower.add_cash(amount);

		Loan loan;
		loan.kind = Loan::kind_t::EXTERNAL;
		loan.lender = &lender;
		loan.borrower = &borrower;
		loan.principal = loan.remaining = amount;
		loan.annual_rate = offer.rate;
		loan.term = term;
		loan.start_date = instance_manager.get_today();
		loan.grace_end = loan.start_date;
		loan.due_date = loan.start_date + Loan::get_term_length(term);
		add_loan(std::move(loan));
		return true;
	}
	return false;
}

bool CreditManager::ai_accepts_loan(
	InstanceManager& instance_manager, CountryInstance& borrower, CountryInstance& lender, const fixed_point_t amount,
	const fixed_point_t rate, const bool allow_reactions
) {
	if (!borrower.is_ai() || !is_valid_country(borrower) || borrower.is_at_war_with(lender)) {
		return false;
	}

	const bool needs_money = borrower.get_cash_stockpile().load() < AI_NEEDS_MONEY_BELOW;
	const bool can_afford = get_total_debt(borrower) + amount <= get_debt_limit(borrower);
	const bool rate_ok = rate <= AI_MAX_ACCEPTED_RATE;
	const std::int32_t relations = static_cast<std::int32_t>(borrower.get_relations_with(lender));

	// Countries do not take money from those they dislike
	if (needs_money && can_afford && rate_ok && relations >= 0) {
		return true;
	}

	// A country that needs the money but does not trust the lender tries to get closer to it, and a small country that
	// already is a friend asks the (human) lender for an alliance
	if (allow_reactions && needs_money && can_afford && rate_ok) {
		change_relations(borrower, lender, 25);
		if (!lender.is_ai() && !borrower.has_alliance_with(lender)
			&& static_cast<std::int32_t>(borrower.get_relations_with(lender)) >= FRIEND_RELATIONS) {
			const bool already_asked = std::any_of(
				alliance_requests.begin(), alliance_requests.end(), [&](AllianceRequest const& request) -> bool {
					return request.from == &borrower && request.to == &lender;
				}
			);
			if (!already_asked) {
				alliance_requests.push_back({ &borrower, &lender, instance_manager.get_today() });
			}
		}
	}
	return false;
}

bool CreditManager::give_external_loan(
	InstanceManager& instance_manager, CountryInstance& lender, CountryInstance& borrower, const fixed_point_t amount,
	fixed_point_t rate, const Loan::term_t term
) {
	rate = clamp_rate(rate);
	if (&lender == &borrower || !is_valid_country(lender) || !is_valid_country(borrower) || amount < MIN_LOAN
		|| lender.get_spendable_cash() < amount) {
		return false;
	}

	if (!ai_accepts_loan(instance_manager, borrower, lender, amount, rate, true)) {
		return false;
	}

	lender.spend_cash(amount);
	borrower.add_cash(amount);

	Loan loan;
	loan.kind = Loan::kind_t::EXTERNAL;
	loan.lender = &lender;
	loan.borrower = &borrower;
	loan.principal = loan.remaining = amount;
	loan.annual_rate = rate;
	loan.term = term;
	loan.start_date = instance_manager.get_today();
	loan.grace_end = loan.start_date;
	loan.due_date = loan.start_date + Loan::get_term_length(term);
	add_loan(std::move(loan));
	return true;
}

std::size_t CreditManager::give_external_loans_to_group(
	InstanceManager& instance_manager, CountryInstance& lender, const target_group_t group, const fixed_point_t amount_each,
	const fixed_point_t rate, const Loan::term_t term
) {
	memory::vector<CountryInstance*> targets;
	if (group == target_group_t::SELECTED) {
		targets = get_staged_targets(lender);
	} else {
		for (CountryInstance& country : instance_manager.get_country_instance_manager().get_country_instances()) {
			if (&country == &lender || !is_valid_country(country)) {
				continue;
			}
			switch (group) {
			case target_group_t::ALL:
				targets.push_back(&country);
				break;
			case target_group_t::ALLIES:
				if (lender.has_alliance_with(country)) {
					targets.push_back(&country);
				}
				break;
			case target_group_t::FRIENDS:
				if (static_cast<std::int32_t>(lender.get_relations_with(country)) >= FRIEND_RELATIONS) {
					targets.push_back(&country);
				}
				break;
			case target_group_t::SELECTED:
				break;
			}
		}
	}

	std::size_t accepted = 0;
	for (CountryInstance* target : targets) {
		if (give_external_loan(instance_manager, lender, *target, amount_each, rate, term)) {
			accepted++;
		}
	}
	return accepted;
}

void CreditManager::stage_loan_target(CountryInstance const& lender, CountryInstance& target, const bool selected) {
	memory::vector<CountryInstance*>& list = staged_targets[&lender];
	const auto it = std::find(list.begin(), list.end(), &target);
	if (selected && it == list.end()) {
		list.push_back(&target);
	} else if (!selected && it != list.end()) {
		list.erase(it);
	}
}

memory::vector<CountryInstance*> const& CreditManager::get_staged_targets(CountryInstance const& lender) const {
	static const memory::vector<CountryInstance*> empty;
	const auto it = staged_targets.find(&lender);
	return it != staged_targets.end() ? it->second : empty;
}

bool CreditManager::answer_alliance_request(
	InstanceManager& instance_manager, CountryInstance& to, CountryInstance& from, const bool accept
) {
	const auto it = std::find_if(alliance_requests.begin(), alliance_requests.end(), [&](AllianceRequest const& request) -> bool {
		return request.from == &from && request.to == &to;
	});
	if (it == alliance_requests.end()) {
		return false;
	}
	alliance_requests.erase(it);

	if (accept) {
		to.set_alliance_with(from, true);
		from.set_alliance_with(to, true);
	} else {
		change_relations(from, to, -10);
	}
	return true;
}

bool CreditManager::give_internal_loan(
	InstanceManager& instance_manager, CountryInstance& lender, State& state, const fixed_point_t amount,
	fixed_point_t rate, const Loan::term_t term, const Loan::maturity_policy_t policy
) {
	if (state.get_owner() != &lender || amount < MIN_LOAN || lender.get_spendable_cash() < amount) {
		return false;
	}
	if (count_capitalists(instance_manager, state) <= 0) {
		return false;
	}

	// Loans are given once a month at most
	const std::int32_t month = month_key(instance_manager.get_today());
	const auto last = last_internal_loan_month.find(&lender);
	if (last != last_internal_loan_month.end() && last->second == month) {
		return false;
	}
	last_internal_loan_month[&lender] = month;

	rate = clamp_rate(rate);
	lender.spend_cash(amount);
	state.set_credit_manager(this);

	Loan loan;
	loan.kind = Loan::kind_t::INTERNAL;
	loan.lender = &lender;
	loan.borrower = &lender;
	loan.state = &state;
	loan.principal = loan.remaining = loan.unspent = amount;
	loan.annual_rate = rate;
	loan.term = term;
	loan.policy = policy;
	loan.start_date = instance_manager.get_today();
	loan.grace_end = loan.start_date + Loan::get_grace_length(term);
	loan.due_date = loan.start_date + Loan::get_term_length(term);
	add_loan(std::move(loan));
	return true;
}

fixed_point_t CreditManager::repay_loan(InstanceManager& instance_manager, Loan& loan, fixed_point_t amount) {
	if (!loan.active || amount <= fixed_point_t::_0) {
		return fixed_point_t::_0;
	}
	amount = std::min(amount, loan.remaining);

	fixed_point_t paid = 0;
	if (loan.kind == Loan::kind_t::EXTERNAL) {
		paid = std::min(amount, loan.borrower->get_spendable_cash());
		loan.borrower->spend_cash(paid);
		loan.lender->add_cash(paid);
	} else {
		paid = loan.state->take_from_capitalist_fund(amount);
		loan.lender->add_cash(paid);
	}

	loan.remaining -= paid;
	if (loan.remaining < fixed_point_t(1) / 100) {
		loan.remaining = fixed_point_t::_0;
		loan.active = false;
		if (loan.kind == Loan::kind_t::INTERNAL && loan.unspent > fixed_point_t::_0) {
			loan.state->add_to_capitalist_fund(loan.unspent);
			loan.unspent = fixed_point_t::_0;
		}
	}
	return paid;
}

bool CreditManager::repay_loan_by_id(
	InstanceManager& instance_manager, CountryInstance& payer, const std::uint32_t id, const fixed_point_t amount
) {
	for (std::unique_ptr<Loan>& loan : loans) {
		if (loan->id == id) {
			if (loan->kind != Loan::kind_t::EXTERNAL || loan->borrower != &payer) {
				return false;
			}
			return repay_loan(instance_manager, *loan, amount) > fixed_point_t::_0;
		}
	}
	return false;
}

fixed_point_t CreditManager::get_capitalist_money(
	State const& state, const Loan::purpose_t purpose, const std::int32_t construction_days
) const {
	fixed_point_t total = state.get_capitalist_fund();
	for (std::unique_ptr<Loan> const& loan : loans) {
		if (loan->active && loan->kind == Loan::kind_t::INTERNAL && loan->state == &state
			&& loan->can_fund(purpose, construction_days)) {
			total += loan->unspent;
		}
	}
	return total;
}

fixed_point_t CreditManager::take_capitalist_money(
	State& state, fixed_point_t amount, const Loan::purpose_t purpose, const std::int32_t construction_days
) {
	// The savings of the capitalists first, then the unspent money of the loans (oldest first)
	fixed_point_t taken = state.take_from_capitalist_fund(amount);
	amount -= taken;

	for (std::unique_ptr<Loan>& loan : loans) {
		if (amount <= fixed_point_t::_0) {
			break;
		}
		if (loan->active && loan->kind == Loan::kind_t::INTERNAL && loan->state == &state
			&& loan->can_fund(purpose, construction_days)) {
			const fixed_point_t from_loan = std::min(amount, loan->unspent);
			loan->unspent -= from_loan;
			amount -= from_loan;
			taken += from_loan;
		}
	}
	return taken;
}

// The State methods that go through the credit manager

fixed_point_t State::get_capitalist_money(const Loan::purpose_t purpose, const std::int32_t construction_days) const {
	return credit_manager != nullptr
		? credit_manager->get_capitalist_money(*this, purpose, construction_days)
		: capitalist_fund;
}

fixed_point_t State::take_capitalist_money(
	const fixed_point_t amount, const Loan::purpose_t purpose, const std::int32_t construction_days
) {
	return credit_manager != nullptr
		? credit_manager->take_capitalist_money(*this, amount, purpose, construction_days)
		: take_from_capitalist_fund(amount);
}

void CreditManager::monthly_interest_and_repayment(InstanceManager& instance_manager, Loan& loan) {
	const Date today = instance_manager.get_today();
	const fixed_point_t interest = loan.remaining * loan.annual_rate / 12;

	if (loan.kind == Loan::kind_t::EXTERNAL) {
		CountryInstance& borrower = *loan.borrower;
		if (borrower.get_spendable_cash() >= interest) {
			borrower.spend_cash(interest);
			loan.lender->add_cash(interest);
			loan.interest_paid += interest;
		} else {
			// What cannot be paid is added to the debt
			loan.remaining += interest;
		}

		// AI countries that have plenty of money pay their debts back
		if (borrower.is_ai() && borrower.get_cash_stockpile().load() > loan.remaining * 3 / 2 + AI_NEEDS_MONEY_BELOW) {
			repay_loan(instance_manager, loan, loan.remaining);
		}
		return;
	}

	// Internal loan: interest comes out of the savings of the capitalists
	State& state = *loan.state;
	const fixed_point_t paid_interest = state.take_from_capitalist_fund(interest);
	loan.lender->add_cash(paid_interest);
	loan.interest_paid += paid_interest;
	loan.remaining += interest - paid_interest;

	if (today >= loan.grace_end) {
		// After the grace period the debt is paid back in equal monthly parts, as far as the capitalists have the money
		const std::int32_t months_left = std::max<std::int32_t>(1, static_cast<std::int32_t>((loan.due_date - today).to_int()) / 30);
		repay_loan(instance_manager, loan, loan.remaining / months_left);
	}

	// Capitalists with a lot of money pay the loan back at once, even in the grace period
	if (loan.active && state.get_capitalist_fund() > loan.remaining * 2) {
		repay_loan(instance_manager, loan, loan.remaining);
	}
}

void CreditManager::seize(InstanceManager& instance_manager, Loan& loan, const bool hard) {
	State& state = *loan.state;
	fixed_point_t deficit = loan.remaining;

	// The money of the loan that was never spent goes back
	const fixed_point_t returned = std::min(deficit, loan.unspent);
	loan.lender->add_cash(returned);
	loan.unspent -= returned;
	deficit -= returned;

	// The savings of the capitalists, then the money of the capitalists themselves
	if (deficit > fixed_point_t::_0) {
		const fixed_point_t from_fund = state.take_from_capitalist_fund(deficit);
		loan.lender->add_cash(from_fund);
		deficit -= from_fund;
	}
	if (deficit > fixed_point_t::_0) {
		if (PopType const* const capitalists = find_capitalists(instance_manager); capitalists != nullptr) {
			for (Pop& pop : state.get_pops_cache_by_type()[capitalists->index]) {
				if (deficit <= fixed_point_t::_0) {
					break;
				}
				const fixed_point_t taken = pop.take_cash(deficit);
				loan.lender->add_cash(taken);
				deficit -= taken;
			}
		}
	}

	// A hard collection takes shares of the factories at their value
	if (hard && deficit > fixed_point_t::_0) {
		MarketInstance const& market = instance_manager.get_market_instance();

		memory::vector<FactoryInstance*> factories;
		for (FactoryInstance& factory : state.get_mutable_factories()) {
			if (factory.get_level() > building_level_t(0) && factory.get_state_share() < fixed_point_t::_1) {
				factories.push_back(&factory);
			}
		}
		std::sort(factories.begin(), factories.end(), [&market](FactoryInstance* lhs, FactoryInstance* rhs) -> bool {
			return lhs->calculate_value(market) > rhs->calculate_value(market);
		});

		for (FactoryInstance* factory : factories) {
			if (deficit <= fixed_point_t::_0) {
				break;
			}
			const fixed_point_t value = factory->calculate_value(market);
			if (value <= fixed_point_t::_0) {
				continue;
			}
			const fixed_point_t private_share = fixed_point_t::_1 - factory->get_state_share();
			const fixed_point_t share = std::min(private_share, deficit / value);
			factory->set_state_share(factory->get_state_share() + share);
			deficit -= share * value;
		}
	}

	// Whatever is left of the debt is written off
	loan.remaining = fixed_point_t::_0;
	loan.active = false;
}

void CreditManager::handle_maturity(InstanceManager& instance_manager, Loan& loan) {
	if (loan.kind == Loan::kind_t::EXTERNAL) {
		const fixed_point_t paid = repay_loan(instance_manager, loan, loan.remaining);
		(void)paid;
		if (loan.active) {
			// The borrower cannot pay: it defaults, the debt is written off and the lender is not happy
			change_relations(*loan.lender, *loan.borrower, -50);
			change_relations(*loan.borrower, *loan.lender, -50);
			loan.remaining = fixed_point_t::_0;
			loan.active = false;
		}
		return;
	}

	switch (loan.policy) {
	case Loan::maturity_policy_t::FORGIVE:
		loan.remaining = fixed_point_t::_0;
		loan.active = false;
		break;
	case Loan::maturity_policy_t::SOFT:
		seize(instance_manager, loan, false);
		break;
	case Loan::maturity_policy_t::HARD:
		seize(instance_manager, loan, true);
		break;
	}
}

void CreditManager::run_ai(InstanceManager& instance_manager) {
	const std::int32_t month = month_key(instance_manager.get_today());
	CountryInstanceManager& country_manager = instance_manager.get_country_instance_manager();

	// Countries with a lot of money lend to their capitalists
	for (CountryInstance& country : country_manager.get_country_instances()) {
		if (!is_valid_country(country) || !country.is_ai() || country.is_at_war()) {
			continue;
		}
		const fixed_point_t cash = country.get_cash_stockpile().load();
		if (cash < fixed_point_t(4000)) {
			continue;
		}
		const auto last = last_internal_loan_month.find(&country);
		if (last != last_internal_loan_month.end() && last->second == month) {
			continue;
		}

		State* best = nullptr;
		fixed_point_t best_money = fixed_point_t(1000);
		for (State* state : country.get_states()) {
			if (count_capitalists(instance_manager, *state) <= 0) {
				continue;
			}
			const fixed_point_t money = state->get_capitalist_money(Loan::purpose_t::CONSTRUCTION, 365);
			if (money < best_money) {
				best_money = money;
				best = state;
			}
		}
		if (best != nullptr) {
			give_internal_loan(
				instance_manager, country, *best, std::min(cash / 10, fixed_point_t(5000)), country.get_internal_loan_rate(),
				Loan::term_t::MEDIUM, Loan::maturity_policy_t::SOFT
			);
		}
	}

	// Countries that need money take the cheapest offer
	for (CountryInstance& country : country_manager.get_country_instances()) {
		if (!is_valid_country(country) || !country.is_ai() || country.get_cash_stockpile().load() >= AI_NEEDS_MONEY_BELOW) {
			continue;
		}
		for (ExternalOffer const& offer : get_offers(instance_manager, country)) {
			if (offer.rate > AI_MAX_ACCEPTED_RATE) {
				break;
			}
			if (take_external_loan(
					instance_manager, country, *offer.lender, std::min(offer.max_amount, fixed_point_t(5000)), Loan::term_t::SHORT
				)) {
				break;
			}
		}
	}
}

void CreditManager::run_capitalist_investment(InstanceManager& instance_manager) {
	MarketInstance const& market = instance_manager.get_market_instance();
	const Date today = instance_manager.get_today();
	BuildingTypeManager const& building_type_manager =
		instance_manager.definition_manager.get_economy_manager().get_building_type_manager();

	for (StateSet& state_set : instance_manager.get_map_instance().get_state_manager().get_mutable_state_sets()) {
		for (State& state : state_set.get_mutable_states()) {
			CountryInstance* const owner = state.get_owner();
			if (owner == nullptr || count_capitalists(instance_manager, state) <= 0) {
				continue;
			}
			// Capitalists who borrowed use the money, those with some savings invest as well
			if (state.get_capitalist_money(Loan::purpose_t::CONSTRUCTION, 365) < fixed_point_t(500)
				&& state.get_capitalist_money(Loan::purpose_t::CONSTRUCTION, 1000) < fixed_point_t(500)) {
				continue;
			}

			// Candidates: the expansion of a profitable factory or a new factory, the best one is chosen by its payback
			FactoryInstance* best_factory = nullptr;
			BuildingType const* best_type = nullptr;
			fixed_point_t best_payback = fixed_point_t(INVESTMENT_MAX_PAYBACK_YEARS);

			for (FactoryInstance& factory : state.get_mutable_factories()) {
				if (!factory.can_be_expanded() || !factory.is_open() || !owner->is_building_type_unlocked(factory.building_type)
					|| factory.get_average_profit() <= fixed_point_t::_0) {
					continue;
				}
				const fixed_point_t cost = FactoryInstance::calculate_construction_cost(factory.building_type, market);
				const fixed_point_t yearly_profit_per_level = factory.get_average_profit() * 365
					/ static_cast<std::int32_t>(type_safe::get(factory.get_level()));
				const fixed_point_t payback = cost / yearly_profit_per_level;
				if (payback < best_payback) {
					best_payback = payback;
					best_factory = &factory;
					best_type = &factory.building_type;
				}
			}

			if (state.has_free_factory_slot()) {
				for (BuildingType const& building_type : building_type_manager.get_building_types()) {
					ProductionType const* const production_type = building_type.production_type;
					if (production_type == nullptr || production_type->template_type != ProductionType::template_type_t::FACTORY
						|| !owner->is_building_type_unlocked(building_type) || state.find_factory(building_type) != nullptr
						|| !production_type->is_valid_for_factory_in(state)) {
						continue;
					}

					// What a level of the factory is expected to earn: the added value, half of which are the wages
					fixed_point_t added_value = production_type->base_output_quantity *
						market.get_good_instance(production_type->output_good.index).get_price();
					for (auto const& [good, quantity] : production_type->input_goods) {
						added_value -= quantity * market.get_good_instance(good->index).get_price();
					}
					if (added_value <= fixed_point_t::_0) {
						continue;
					}
					const fixed_point_t yearly_profit = added_value / 2 * 365;
					const fixed_point_t payback = FactoryInstance::calculate_construction_cost(building_type, market) / yearly_profit;
					if (payback < best_payback) {
						best_payback = payback;
						best_factory = nullptr;
						best_type = &building_type;
					}
				}
			}

			if (best_type == nullptr) {
				continue;
			}

			const fixed_point_t cost = FactoryInstance::calculate_construction_cost(*best_type, market);
			const std::int32_t days = static_cast<std::int32_t>(FactoryInstance::calculate_construction_time(*best_type).to_int());
			if (state.get_capitalist_money(Loan::purpose_t::CONSTRUCTION, days) < cost) {
				continue;
			}
			if (state.take_capitalist_money(cost, Loan::purpose_t::CONSTRUCTION, days) < cost * fixed_point_t(99) / 100) {
				continue;
			}

			if (best_factory != nullptr) {
				// The capitalists pay for the new level, so the share of the state is diluted
				const std::int32_t level = static_cast<std::int32_t>(type_safe::get(best_factory->get_level()));
				best_factory->set_state_share(best_factory->get_state_share() * level / (level + 1));
				best_factory->start_construction(today);
			} else if (FactoryInstance* const factory = state.add_factory(*best_type, building_level_t(0)); factory != nullptr) {
				factory->set_state_share(fixed_point_t::_0);
				factory->set_built_by_state(false);
				factory->start_construction(today);
			}
		}
	}
}

void CreditManager::tick(InstanceManager& instance_manager) {
	const Date today = instance_manager.get_today();

	// Loans that are due
	for (std::unique_ptr<Loan>& loan : loans) {
		if (loan->active && today >= loan->due_date) {
			handle_maturity(instance_manager, *loan);
		}
	}

	if (today.is_month_start()) {
		for (std::unique_ptr<Loan>& loan : loans) {
			if (loan->active) {
				monthly_interest_and_repayment(instance_manager, *loan);
			}
		}

		run_ai(instance_manager);
		run_capitalist_investment(instance_manager);

		// Finished loans are forgotten
		loans.erase(
			std::remove_if(loans.begin(), loans.end(), [](std::unique_ptr<Loan> const& loan) -> bool {
				return !loan->active;
			}),
			loans.end()
		);
	}
}

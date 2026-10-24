#include "Credit.hpp"

#include <algorithm>
#include <cstdlib>

#include "openvic-simulation/DefinitionManager.hpp"
#include "openvic-simulation/InstanceManager.hpp"
#include "openvic-simulation/country/CountryInstance.hpp"
#include "openvic-simulation/defines/OpenVicDefines.hpp"
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

	std::int32_t days_between(Date const from, Date const to) {
		return static_cast<std::int32_t>((to - from).to_int());
	}
}

// ---------------------------------------------------------------------------------------------------------------------
// The schedule of one credit (no access to the game, so that the formulas can be tested on their own)
// ---------------------------------------------------------------------------------------------------------------------

bool Loan::may_be_taken_by(CountryInstance const& country) const {
	if (status != status_t::OFFERED || kind != kind_t::EXTERNAL || &country == lender) {
		return false;
	}
	if (target_countries.empty()) {
		return true;
	}
	return std::find(target_countries.begin(), target_countries.end(), &country) != target_countries.end();
}

bool Loan::can_fund(const purpose_t purpose, const std::int32_t construction_days, const std::int32_t short_max_days) const {
	if (term != term_t::SHORT) {
		return true;
	}
	return purpose == purpose_t::CONSTRUCTION && construction_days > 0 && construction_days <= short_max_days;
}

fixed_point_t Loan::calculate_total_interest(
	const fixed_point_t principal, const fixed_point_t annual_rate, const std::int32_t delay_days, const std::int32_t repayment_days
) {
	// principal * rate * (delay + repayment / 2) years; the days are whole numbers of at most 36500 so the product is exact
	const std::int32_t weighted_days = delay_days + repayment_days / 2;
	return principal * annual_rate * weighted_days / 365;
}

std::int32_t Loan::calculate_installments(const std::int32_t term_days, const std::int32_t delay_days) {
	const std::int32_t repayment_days = std::max(term_days - delay_days, 1);
	return std::max<std::int32_t>(1, (repayment_days + CreditManager::DAYS_PER_INSTALLMENT - 1) / CreditManager::DAYS_PER_INSTALLMENT);
}

void Loan::start_schedule(const Date today) {
	status = status_t::ACTIVE;
	taken_date = today;
	accrual_from = today;
	payments_start = today + Timespan::from_days(delay_days);
	due_date = today + Timespan::from_days(term_days);
	principal_left = principal;
	interest_locked = fixed_point_t::_0;
	installments_left = calculate_installments(term_days, delay_days);
	interest_left = calculate_total_interest(principal, annual_rate, delay_days, std::max(term_days - delay_days, 0));
}

fixed_point_t Loan::get_installment_due(const Date today) const {
	if (status != status_t::ACTIVE || today < payments_start || installments_left <= 0) {
		return fixed_point_t::_0;
	}
	return (principal_left + interest_left) / installments_left;
}

void Loan::apply_installment_payment(const fixed_point_t due, const fixed_point_t paid) {
	if (due <= fixed_point_t::_0) {
		return;
	}
	if (paid <= fixed_point_t::_0) {
		installments_left = std::max(installments_left - 1, 0);
		return;
	}
	// The payment is split in the proportion of the schedule: the same share of the principal and of the interest
	const fixed_point_t whole = principal_left + interest_left;
	const fixed_point_t principal_part = whole > fixed_point_t::_0 ? paid * principal_left / whole : fixed_point_t::_0;
	const fixed_point_t interest_part = paid - principal_part;
	principal_left -= std::min(principal_part, principal_left);
	interest_left -= std::min(interest_part, interest_left);
	principal_paid += principal_part;
	interest_paid += interest_part;
	// The month is over whether the installment was paid or not: what was not paid is spread over the months that are left
	installments_left = std::max(installments_left - 1, 0);
	if (get_remaining() < fixed_point_t(1) / 100) {
		principal_left = fixed_point_t::_0;
		interest_left = fixed_point_t::_0;
		installments_left = 0;
	}
}

fixed_point_t Loan::get_full_repayment_cost(const Date today) const {
	fixed_point_t cost = principal_left;
	if (today < payments_start) {
		// What the principal has earned in the delay so far
		cost += interest_locked + principal_left * annual_rate * days_between(accrual_from, today) / 365;
	}
	return cost;
}

fixed_point_t Loan::apply_early_repayment(const Date today, fixed_point_t amount) {
	if (status != status_t::ACTIVE || amount <= fixed_point_t::_0) {
		return fixed_point_t::_0;
	}

	const fixed_point_t full_cost = get_full_repayment_cost(today);
	if (amount >= full_cost) {
		// Everything is paid back: the interest of the future is not owed
		const fixed_point_t interest_part = full_cost - principal_left;
		principal_paid += principal_left;
		interest_paid += interest_part;
		principal_left = fixed_point_t::_0;
		interest_left = fixed_point_t::_0;
		installments_left = 0;
		return full_cost;
	}

	// A part of the principal: the interest earned so far stays owed, the future interest is calculated again for what is left
	amount = std::min(amount, principal_left);
	const fixed_point_t new_principal = principal_left - amount;
	principal_paid += amount;

	if (today < payments_start) {
		interest_locked += principal_left * annual_rate * days_between(accrual_from, today) / 365;
		accrual_from = today;
		const std::int32_t repayment_days = std::max(term_days - delay_days, 0);
		const std::int32_t delay_left = days_between(today, payments_start);
		interest_left = interest_locked + calculate_total_interest(new_principal, annual_rate, delay_left, repayment_days);
	} else {
		interest_left = calculate_total_interest(new_principal, annual_rate, 0, installments_left * CreditManager::DAYS_PER_INSTALLMENT);
	}
	principal_left = new_principal;
	return amount;
}

// ---------------------------------------------------------------------------------------------------------------------
// The credit manager
// ---------------------------------------------------------------------------------------------------------------------

fixed_point_t CreditManager::get_debt_limit(OpenVicCreditDefines const& defines, CountryInstance const& country) {
	const std::int64_t population = static_cast<std::int64_t>(type_safe::get(country.get_total_population()));
	return fixed_point_t(defines.debt_limit_base)
		+ fixed_point_t(static_cast<std::int32_t>(std::min<std::int64_t>(population / defines.debt_limit_pop_divisor, 1000000000)));
}

std::int32_t CreditManager::get_term_days(const Loan::term_t term) const {
	switch (term) {
	case Loan::term_t::SHORT:  return defines->term_short_days;
	case Loan::term_t::MEDIUM: return defines->term_medium_days;
	case Loan::term_t::LONG:   return defines->term_long_days;
	}
	return defines->term_short_days;
}

std::int32_t CreditManager::get_delay_days(const Loan::term_t term) const {
	// The delay never takes the whole term: at least one payment is left
	std::int32_t delay = defines->delay_short_days;
	switch (term) {
	case Loan::term_t::SHORT:  delay = defines->delay_short_days; break;
	case Loan::term_t::MEDIUM: delay = defines->delay_medium_days; break;
	case Loan::term_t::LONG:   delay = defines->delay_long_days; break;
	}
	return std::min(delay, get_term_days(term) - DAYS_PER_INSTALLMENT);
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
		if (loan->status == Loan::status_t::ACTIVE && loan->kind == Loan::kind_t::EXTERNAL && loan->borrower == &borrower) {
			total += loan->get_remaining();
		}
	}
	return total;
}

fixed_point_t CreditManager::get_monthly_payments(CountryInstance const& borrower, const Date today) const {
	fixed_point_t total = 0;
	for (std::unique_ptr<Loan> const& loan : loans) {
		if (loan->status == Loan::status_t::ACTIVE && loan->kind == Loan::kind_t::EXTERNAL && loan->borrower == &borrower) {
			total += loan->get_installment_due(today);
		}
	}
	return total;
}

// --- targets chosen in the tables ---

void CreditManager::stage_target_country(CountryInstance const& lender, CountryInstance& target, const bool selected) {
	memory::vector<CountryInstance*>& list = staged_countries[&lender];
	const auto it = std::find(list.begin(), list.end(), &target);
	if (selected && it == list.end()) {
		list.push_back(&target);
	} else if (!selected && it != list.end()) {
		list.erase(it);
	}
}

void CreditManager::stage_target_state(CountryInstance const& lender, State& target, const bool selected) {
	memory::vector<State*>& list = staged_states[&lender];
	const auto it = std::find(list.begin(), list.end(), &target);
	if (selected && it == list.end()) {
		list.push_back(&target);
	} else if (!selected && it != list.end()) {
		list.erase(it);
	}
}

void CreditManager::clear_staged(CountryInstance const& lender) {
	staged_countries.erase(&lender);
	staged_states.erase(&lender);
}

memory::vector<CountryInstance*> const& CreditManager::get_staged_countries(CountryInstance const& lender) const {
	static const memory::vector<CountryInstance*> empty;
	const auto it = staged_countries.find(&lender);
	return it != staged_countries.end() ? it->second : empty;
}

memory::vector<State*> const& CreditManager::get_staged_states(CountryInstance const& lender) const {
	static const memory::vector<State*> empty;
	const auto it = staged_states.find(&lender);
	return it != staged_states.end() ? it->second : empty;
}

// --- offers ---

std::int32_t CreditManager::count_open_offers(CountryInstance const& lender, const Loan::kind_t kind) const {
	std::int32_t count = 0;
	for (std::unique_ptr<Loan> const& loan : loans) {
		if (loan->status == Loan::status_t::OFFERED && loan->lender == &lender && loan->kind == kind) {
			count++;
		}
	}
	return count;
}

Loan* CreditManager::offer_internal_credit(
	InstanceManager& instance_manager, CountryInstance& lender, const fixed_point_t amount, const fixed_point_t rate,
	const Loan::term_t term, const bool all_regions, const bool as_ai
) {
	if (!is_valid_country(lender) || amount < fixed_point_t(defines->min_loan) || lender.get_spendable_cash() < amount
		|| rate <= fixed_point_t::_0 || rate > defines->interest_limit.fixed()) {
		return nullptr;
	}

	memory::vector<State*> targets;
	if (!all_regions) {
		targets = get_staged_states(lender);
		if (targets.empty()) {
			return nullptr;
		}
		// Only regions of the lender with capitalists
		targets.erase(
			std::remove_if(targets.begin(), targets.end(), [&](State* state) -> bool {
				return state->get_owner() != &lender || count_capitalists(instance_manager, *state) <= 0;
			}),
			targets.end()
		);
		if (targets.empty()) {
			return nullptr;
		}
	}

	lender.spend_cash(amount);

	Loan loan;
	loan.kind = Loan::kind_t::INTERNAL;
	loan.status = Loan::status_t::OFFERED;
	loan.lender = &lender;
	loan.target_states = std::move(targets);
	loan.principal = loan.principal_left = amount;
	loan.annual_rate = rate;
	loan.term = term;
	loan.term_days = get_term_days(term);
	loan.delay_days = get_delay_days(term);
	loan.issue_date = instance_manager.get_today();
	if (as_ai) {
		loan.has_expiry = true;
		loan.expiry_date = loan.issue_date + Timespan::from_days(defines->ai_offer_expiry_days);
	}
	clear_staged(lender);
	return &add_loan(std::move(loan));
}

Loan* CreditManager::offer_external_credit(
	InstanceManager& instance_manager, CountryInstance& lender, const fixed_point_t amount, const fixed_point_t rate,
	const Loan::term_t term, const bool all_countries, const bool as_ai
) {
	if (!is_valid_country(lender) || amount < fixed_point_t(defines->min_loan) || lender.get_spendable_cash() < amount
		|| rate <= fixed_point_t::_0 || rate > defines->interest_limit.fixed()) {
		return nullptr;
	}

	memory::vector<CountryInstance*> targets;
	if (!all_countries) {
		targets = get_staged_countries(lender);
		targets.erase(
			std::remove_if(targets.begin(), targets.end(), [&](CountryInstance* country) -> bool {
				return country == &lender || !is_valid_country(*country);
			}),
			targets.end()
		);
		if (targets.empty()) {
			return nullptr;
		}
	}

	lender.spend_cash(amount);

	Loan loan;
	loan.kind = Loan::kind_t::EXTERNAL;
	loan.status = Loan::status_t::OFFERED;
	loan.lender = &lender;
	loan.target_countries = std::move(targets);
	loan.principal = loan.principal_left = amount;
	loan.annual_rate = rate;
	loan.term = term;
	loan.term_days = get_term_days(term);
	loan.delay_days = get_delay_days(term);
	loan.issue_date = instance_manager.get_today();
	if (as_ai) {
		loan.has_expiry = true;
		loan.expiry_date = loan.issue_date + Timespan::from_days(defines->ai_offer_expiry_days);
	}
	clear_staged(lender);
	return &add_loan(std::move(loan));
}

bool CreditManager::withdraw_offer(InstanceManager& instance_manager, CountryInstance& lender, const std::uint32_t id) {
	for (std::unique_ptr<Loan>& loan : loans) {
		if (loan->id == id) {
			if (loan->status != Loan::status_t::OFFERED || loan->lender != &lender) {
				return false;
			}
			close(*loan, Loan::end_t::WITHDRAWN);
			return true;
		}
	}
	return false;
}

void CreditManager::close(Loan& loan, const Loan::end_t how) {
	if (loan.status == Loan::status_t::OFFERED && loan.lender != nullptr && loan.principal > fixed_point_t::_0
		&& (how == Loan::end_t::WITHDRAWN || how == Loan::end_t::EXPIRED)) {
		// The money that was held for the offer goes back to the lender
		loan.lender->add_cash(loan.principal);
	}
	loan.status = Loan::status_t::CLOSED;
	loan.end = how;
}

bool CreditManager::lender_accepts(InstanceManager const& instance_manager, Loan const& loan, CountryInstance const& borrower) const {
	if (loan.lender->is_at_war_with(borrower)) {
		return false;
	}
	// An AI lender does not lend to a country that already owes too much or that it does not like
	if (loan.lender->is_ai()) {
		const fixed_point_t limit = get_debt_limit(*defines, borrower);
		if (limit > fixed_point_t::_0
			&& (get_total_debt(borrower) + loan.principal) / limit > defines->ai_lender_max_debt_ratio.fixed()) {
			return false;
		}
		if (static_cast<std::int32_t>(loan.lender->get_relations_with(borrower)) < defines->ai_min_relations) {
			return false;
		}
	}
	return true;
}

void CreditManager::activate(InstanceManager& instance_manager, Loan& loan, CountryInstance* const borrower, State* const state) {
	loan.borrower = borrower;
	loan.state = state;
	loan.start_schedule(instance_manager.get_today());
	if (loan.kind == Loan::kind_t::EXTERNAL) {
		borrower->add_cash(loan.principal);
	} else {
		// The capitalists spend it on factories (take_capitalist_money)
		loan.unspent = loan.principal;
		state->set_credit_manager(this);
	}
}

bool CreditManager::take_credit(InstanceManager& instance_manager, CountryInstance& borrower, const std::uint32_t id) {
	for (std::unique_ptr<Loan>& loan : loans) {
		if (loan->id != id) {
			continue;
		}
		if (!is_valid_country(borrower) || !loan->may_be_taken_by(borrower) || !is_valid_country(*loan->lender)) {
			return false;
		}
		if (!lender_accepts(instance_manager, *loan, borrower)) {
			return false;
		}
		if (get_total_debt(borrower) + loan->principal > get_debt_limit(*defines, borrower)) {
			return false;
		}
		activate(instance_manager, *loan, &borrower, nullptr);
		return true;
	}
	return false;
}

bool CreditManager::take_internal_credit(InstanceManager& instance_manager, const std::uint32_t id, State& state) {
	for (std::unique_ptr<Loan>& loan : loans) {
		if (loan->id != id) {
			continue;
		}
		if (loan->status != Loan::status_t::OFFERED || loan->kind != Loan::kind_t::INTERNAL || loan->lender != state.get_owner()
			|| !is_valid_country(*loan->lender)) {
			return false;
		}
		if (!loan->target_states.empty()
			&& std::find(loan->target_states.begin(), loan->target_states.end(), &state) == loan->target_states.end()) {
			return false;
		}
		activate(instance_manager, *loan, loan->lender, &state);
		return true;
	}
	return false;
}

memory::vector<Loan const*> CreditManager::get_available_offers(
	InstanceManager const& instance_manager, CountryInstance const& borrower
) const {
	memory::vector<Loan const*> offers;
	for (std::unique_ptr<Loan> const& loan : loans) {
		if (loan->may_be_taken_by(borrower) && is_valid_country(*loan->lender) && lender_accepts(instance_manager, *loan, borrower)
			&& get_total_debt(borrower) + loan->principal <= get_debt_limit(*defines, borrower)) {
			offers.push_back(loan.get());
		}
	}
	std::sort(offers.begin(), offers.end(), [](Loan const* lhs, Loan const* rhs) -> bool {
		return lhs->annual_rate != rhs->annual_rate ? lhs->annual_rate < rhs->annual_rate : lhs->id < rhs->id;
	});
	return offers;
}

// --- payments ---

fixed_point_t CreditManager::pay(Loan& loan, const fixed_point_t amount) {
	if (amount <= fixed_point_t::_0) {
		return fixed_point_t::_0;
	}
	fixed_point_t paid = 0;
	if (loan.kind == Loan::kind_t::EXTERNAL) {
		paid = std::min(amount, loan.borrower->get_spendable_cash());
		if (paid > fixed_point_t::_0) {
			loan.borrower->spend_cash(paid);
			loan.lender->add_cash(paid);
		}
	} else {
		paid = loan.state->take_from_capitalist_fund(amount);
		if (paid > fixed_point_t::_0) {
			loan.lender->add_cash(paid);
		}
	}
	return paid;
}

fixed_point_t CreditManager::repay_loan(InstanceManager& instance_manager, Loan& loan, const fixed_point_t amount) {
	if (loan.status != Loan::status_t::ACTIVE || amount <= fixed_point_t::_0) {
		return fixed_point_t::_0;
	}
	const Date today = instance_manager.get_today();
	// How much the borrower can really pay now
	const fixed_point_t available = std::min(amount, loan.kind == Loan::kind_t::EXTERNAL
		? loan.borrower->get_spendable_cash()
		: loan.state->get_capitalist_fund());
	if (available <= fixed_point_t::_0) {
		return fixed_point_t::_0;
	}
	const fixed_point_t applied = loan.apply_early_repayment(today, available);
	const fixed_point_t paid = pay(loan, applied);

	if (loan.principal_left <= fixed_point_t::_0) {
		if (loan.kind == Loan::kind_t::INTERNAL && loan.unspent > fixed_point_t::_0) {
			// The money the capitalists did not spend goes back with the credit
			loan.lender->add_cash(loan.unspent);
			loan.unspent = fixed_point_t::_0;
		}
		close(loan, Loan::end_t::PAID);
	}
	return paid;
}

bool CreditManager::repay_loan_by_id(
	InstanceManager& instance_manager, CountryInstance& payer, const std::uint32_t id, const fixed_point_t amount
) {
	for (std::unique_ptr<Loan>& loan : loans) {
		if (loan->id == id) {
			if (loan->kind != Loan::kind_t::EXTERNAL || loan->borrower != &payer || loan->status != Loan::status_t::ACTIVE) {
				return false;
			}
			return repay_loan(instance_manager, *loan, amount) > fixed_point_t::_0;
		}
	}
	return false;
}

// --- the money of the capitalists ---

fixed_point_t CreditManager::get_capitalist_money(
	State const& state, const Loan::purpose_t purpose, const std::int32_t construction_days
) const {
	fixed_point_t total = state.get_capitalist_fund();
	for (std::unique_ptr<Loan> const& loan : loans) {
		if (loan->status == Loan::status_t::ACTIVE && loan->kind == Loan::kind_t::INTERNAL && loan->state == &state
			&& loan->can_fund(purpose, construction_days, defines->short_max_construction_days)) {
			total += loan->unspent;
		}
	}
	return total;
}

fixed_point_t CreditManager::take_capitalist_money(
	State& state, fixed_point_t amount, const Loan::purpose_t purpose, const std::int32_t construction_days
) {
	// The savings of the capitalists first, then the unspent money of the credits (oldest first)
	fixed_point_t taken = state.take_from_capitalist_fund(amount);
	amount -= taken;

	for (std::unique_ptr<Loan>& loan : loans) {
		if (amount <= fixed_point_t::_0) {
			break;
		}
		if (loan->status == Loan::status_t::ACTIVE && loan->kind == Loan::kind_t::INTERNAL && loan->state == &state
			&& loan->can_fund(purpose, construction_days, defines->short_max_construction_days)) {
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

// --- the month ---

void CreditManager::check_parties(InstanceManager& instance_manager, Loan& loan) {
	if (loan.status != Loan::status_t::ACTIVE) {
		return;
	}
	if (!is_valid_country(*loan.lender)) {
		// The lender is gone: nobody collects the debt. The money of an internal credit that was not spent is lost with it.
		loan.unspent = fixed_point_t::_0;
		close(loan, Loan::end_t::WRITTEN_OFF);
		return;
	}
	if (loan.kind == Loan::kind_t::EXTERNAL) {
		// The borrower is bankrupt (the country does not exist any more): the debt is written off
		if (!is_valid_country(*loan.borrower)) {
			close(loan, Loan::end_t::WRITTEN_OFF);
		}
		return;
	}
	// Internal: the credit goes with the region. When the region changes its owner, the new owner is the lender.
	CountryInstance* const owner = loan.state->get_owner();
	if (owner == nullptr || !is_valid_country(*owner)) {
		loan.unspent = fixed_point_t::_0;
		close(loan, Loan::end_t::WRITTEN_OFF);
	} else if (owner != loan.lender) {
		loan.lender = owner;
		loan.borrower = owner;
	}
}

void CreditManager::process_monthly(InstanceManager& instance_manager, Loan& loan) {
	const Date today = instance_manager.get_today();

	const fixed_point_t due = loan.get_installment_due(today);
	if (due > fixed_point_t::_0) {
		const fixed_point_t paid = pay(loan, due);
		loan.apply_installment_payment(due, paid);
		if (loan.principal_left <= fixed_point_t::_0 && loan.interest_left <= fixed_point_t::_0) {
			if (loan.kind == Loan::kind_t::INTERNAL && loan.unspent > fixed_point_t::_0) {
				loan.lender->add_cash(loan.unspent);
				loan.unspent = fixed_point_t::_0;
			}
			close(loan, Loan::end_t::PAID);
			return;
		}
	}

	if (loan.kind == Loan::kind_t::EXTERNAL) {
		// AI countries that have plenty of money pay their debts back early
		CountryInstance& borrower = *loan.borrower;
		if (borrower.is_ai()) {
			const fixed_point_t cost = loan.get_full_repayment_cost(today);
			if (borrower.get_cash_stockpile().load() > cost * 3 / 2 + fixed_point_t(defines->ai_early_repay_margin)) {
				repay_loan(instance_manager, loan, cost);
			}
		}
	} else if (loan.status == Loan::status_t::ACTIVE) {
		// Capitalists with a lot of money pay the credit back at once
		const fixed_point_t cost = loan.get_full_repayment_cost(today);
		if (cost > fixed_point_t::_0 && loan.state->get_capitalist_fund() > cost * 2) {
			repay_loan(instance_manager, loan, cost);
		}
	}
}

void CreditManager::seize(InstanceManager& instance_manager, Loan& loan) {
	State& state = *loan.state;
	fixed_point_t deficit = loan.get_remaining();

	// The money of the credit that was never spent goes back
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
	// Whatever is left is written off
	if (loan.unspent > fixed_point_t::_0) {
		loan.lender->add_cash(loan.unspent);
		loan.unspent = fixed_point_t::_0;
	}
}

void CreditManager::handle_due(InstanceManager& instance_manager, Loan& loan) {
	if (loan.kind == Loan::kind_t::EXTERNAL) {
		// The last payment: everything that is still owed (the principal and the interest that was not paid in time)
		const fixed_point_t owed = loan.get_remaining();
		const fixed_point_t paid = pay(loan, owed);
		loan.apply_installment_payment(owed, paid);
		if (loan.get_remaining() <= fixed_point_t::_0) {
			close(loan, Loan::end_t::PAID);
			return;
		}
		if (loan.status == Loan::status_t::ACTIVE) {
			// The borrower cannot pay: it defaults, the debt is written off and the relations suffer
			change_relations(*loan.lender, *loan.borrower, -50);
			change_relations(*loan.borrower, *loan.lender, -50);
			loan.principal_left = fixed_point_t::_0;
			loan.interest_left = fixed_point_t::_0;
			close(loan, Loan::end_t::DEFAULTED);
		}
		return;
	}
	seize(instance_manager, loan);
	loan.principal_left = fixed_point_t::_0;
	loan.interest_left = fixed_point_t::_0;
	close(loan, Loan::end_t::DEFAULTED);
}

void CreditManager::expire_offers(InstanceManager& instance_manager) {
	const Date today = instance_manager.get_today();
	for (std::unique_ptr<Loan>& loan : loans) {
		if (loan->status == Loan::status_t::OFFERED) {
			if (!is_valid_country(*loan->lender)) {
				// The lender is gone, the held money is lost with it
				loan->principal = fixed_point_t::_0;
				close(*loan, Loan::end_t::EXPIRED);
			} else if (loan->has_expiry && today >= loan->expiry_date) {
				close(*loan, Loan::end_t::EXPIRED);
			}
		}
	}
}

// --- AI ---

void CreditManager::run_ai_lending(InstanceManager& instance_manager) {
	CountryInstanceManager& country_manager = instance_manager.get_country_instance_manager();
	const fixed_point_t min_rate = defines->interest_min.fixed();

	for (CountryInstance& country : country_manager.get_country_instances()) {
		if (!is_valid_country(country) || !country.is_ai() || country.is_at_war()) {
			continue;
		}
		const fixed_point_t cash = country.get_cash_stockpile().load();

		// 1. Free money that has nowhere to go lends to its own capitalists
		if (cash >= fixed_point_t(defines->ai_internal_lend_cash) && count_open_offers(country, Loan::kind_t::INTERNAL) < defines->ai_max_open_offers) {
			State* best = nullptr;
			fixed_point_t best_money = fixed_point_t(defines->investment_min_money) * 2;
			for (State* state : country.get_states()) {
				if (count_capitalists(instance_manager, *state) <= 0) {
					continue;
				}
				const fixed_point_t money = state->get_capitalist_money(Loan::purpose_t::CONSTRUCTION, defines->short_max_construction_days);
				if (money < best_money) {
					best_money = money;
					best = state;
				}
			}
			if (best != nullptr) {
				stage_target_state(country, *best, true);
				const fixed_point_t amount = std::min(defines->ai_internal_lend_share.of(cash), fixed_point_t(defines->ai_internal_max_loan));
				// An AI country never lends for less than the minimum interest
				const fixed_point_t rate = std::max(country.get_internal_loan_rate(), min_rate);
				offer_internal_credit(instance_manager, country, amount, rate, Loan::term_t::MEDIUM, false, true);
				clear_staged(country);
				continue;
			}
		}

		// 2. Money the country does not need lends to other countries
		const fixed_point_t surplus = cash - fixed_point_t(defines->ai_lend_reserve);
		if (surplus >= fixed_point_t(defines->min_loan) && count_open_offers(country, Loan::kind_t::EXTERNAL) < defines->ai_max_open_offers) {
			const fixed_point_t amount = std::min(defines->ai_lend_share.of(surplus), fixed_point_t(defines->ai_max_loan));
			const fixed_point_t rate = std::max(country.get_external_loan_rate(), min_rate);
			offer_external_credit(instance_manager, country, amount, rate, Loan::term_t::SHORT, true, true);
		}
	}
}

void CreditManager::run_ai_borrowing(InstanceManager& instance_manager) {
	CountryInstanceManager& country_manager = instance_manager.get_country_instance_manager();
	const fixed_point_t max_rate = defines->interest_max.fixed();

	for (CountryInstance& country : country_manager.get_country_instances()) {
		if (!is_valid_country(country) || !country.is_ai()
			|| country.get_cash_stockpile().load() >= fixed_point_t(defines->ai_needs_money_below)) {
			continue;
		}

		// The best offer: the lowest rate, a little lower for those the country likes
		Loan const* best = nullptr;
		fixed_point_t best_score = 0;
		for (Loan const* offer : get_available_offers(instance_manager, country)) {
			// An AI country never takes a credit above the maximum interest
			if (offer->annual_rate > max_rate) {
				continue;
			}
			const std::int32_t relations = static_cast<std::int32_t>(offer->lender->get_relations_with(country));
			if (relations < defines->ai_min_relations) {
				continue;
			}
			const fixed_point_t score = offer->annual_rate - defines->ai_relations_rate_per_point.fixed() * relations;
			if (best == nullptr || score < best_score) {
				best = offer;
				best_score = score;
			}
		}
		if (best != nullptr) {
			take_credit(instance_manager, country, best->id);
		}
	}
}

bool CreditManager::take_internal_offer_for(
	InstanceManager& instance_manager, State& state, const fixed_point_t cost, const std::int32_t construction_days
) {
	CountryInstance* const owner = state.get_owner();
	if (owner == nullptr) {
		return false;
	}
	const fixed_point_t money = state.get_capitalist_money(Loan::purpose_t::CONSTRUCTION, construction_days);

	Loan* best = nullptr;
	for (std::unique_ptr<Loan>& loan : loans) {
		if (loan->status != Loan::status_t::OFFERED || loan->kind != Loan::kind_t::INTERNAL || loan->lender != owner
			|| !loan->can_fund(Loan::purpose_t::CONSTRUCTION, construction_days, defines->short_max_construction_days)) {
			continue;
		}
		// Capitalists do not borrow at a rate above the maximum
		if (loan->annual_rate > defines->interest_max.fixed()) {
			continue;
		}
		if (!loan->target_states.empty()
			&& std::find(loan->target_states.begin(), loan->target_states.end(), &state) == loan->target_states.end()) {
			continue;
		}
		if (money + loan->principal < cost) {
			continue;
		}
		if (best == nullptr || loan->annual_rate < best->annual_rate) {
			best = loan.get();
		}
	}
	if (best == nullptr) {
		return false;
	}
	activate(instance_manager, *best, owner, &state);
	return true;
}

void CreditManager::run_capitalist_investment(InstanceManager& instance_manager) {
	MarketInstance const& market = instance_manager.get_market_instance();
	const Date today = instance_manager.get_today();
	BuildingTypeManager const& building_type_manager =
		instance_manager.definition_manager.get_economy_manager().get_building_type_manager();
	const fixed_point_t min_money = fixed_point_t(defines->investment_min_money);
	const std::int32_t max_payback = defines->investment_max_payback_years;

	bool any_open_internal_offer = false;
	for (std::unique_ptr<Loan> const& loan : loans) {
		if (loan->status == Loan::status_t::OFFERED && loan->kind == Loan::kind_t::INTERNAL) {
			any_open_internal_offer = true;
			break;
		}
	}

	for (StateSet& state_set : instance_manager.get_map_instance().get_state_manager().get_mutable_state_sets()) {
		for (State& state : state_set.get_mutable_states()) {
			CountryInstance* const owner = state.get_owner();
			if (owner == nullptr || count_capitalists(instance_manager, state) <= 0) {
				continue;
			}
			// Capitalists with money invest; those without can take an offered credit
			const bool has_money = state.get_capitalist_money(Loan::purpose_t::CONSTRUCTION, 365) >= min_money
				|| state.get_capitalist_money(Loan::purpose_t::CONSTRUCTION, 1000) >= min_money;
			if (!has_money && !any_open_internal_offer) {
				continue;
			}

			// Candidates: the expansion of a profitable factory or a new factory, the best one is chosen by its payback
			FactoryInstance* best_factory = nullptr;
			BuildingType const* best_type = nullptr;
			fixed_point_t best_payback = fixed_point_t(max_payback);

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
				// Not enough: the capitalists take an offered credit if it makes the project possible
				if (!any_open_internal_offer || !take_internal_offer_for(instance_manager, state, cost, days)) {
					continue;
				}
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

void CreditManager::run_ai(InstanceManager& instance_manager) {
	// Lenders first, so that the offers can be taken in the same month
	run_ai_lending(instance_manager);
	run_ai_borrowing(instance_manager);
}

void CreditManager::tick(InstanceManager& instance_manager) {
	const Date today = instance_manager.get_today();

	// Credits that are due
	for (std::unique_ptr<Loan>& loan : loans) {
		if (loan->status == Loan::status_t::ACTIVE && today >= loan->due_date) {
			handle_due(instance_manager, *loan);
		}
	}

	if (today.is_month_start()) {
		expire_offers(instance_manager);
		for (std::unique_ptr<Loan>& loan : loans) {
			if (loan->status == Loan::status_t::ACTIVE) {
				check_parties(instance_manager, *loan);
			}
			if (loan->status == Loan::status_t::ACTIVE) {
				process_monthly(instance_manager, *loan);
			}
		}

		run_ai(instance_manager);
		run_capitalist_investment(instance_manager);

		// Finished credits are forgotten
		loans.erase(
			std::remove_if(loans.begin(), loans.end(), [](std::unique_ptr<Loan> const& loan) -> bool {
				return loan->status == Loan::status_t::CLOSED;
			}),
			loans.end()
		);
	}
}

#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "openvic-simulation/core/object/FixedPoint.hpp"

namespace OpenVic {
	struct InstanceManager;

	/* Debugging tool for the conservation of money (stage 2 of the plan). Money is only supposed to appear through the
	 * production of gold; every other payment moves money from one holder to another. When the ledger is switched on
	 * (headless -M), it
	 *  - measures the money of all holders (pops, countries, factories, funds of the states) after each phase of the tick and
	 *    adds the change of every phase to the totals of the phase,
	 *  - counts the money moved at the places where the code changes the cash of a holder, by label, as credits and debits.
	 * Nothing here changes the simulation, the hooks do nothing when the ledger is off. */
	struct MoneyLedger {
		// Labels of the hooks: where the money is added to (credit) or taken from (debit) a holder, grouped by category
#define OV_MONEY_LABELS(X) \
	/* taxes: what the pops pay to the country */ \
	X(tax_income, "tax") \
	/* trade: what the buyers pay and what the sellers get at the markets (all goods) */ \
	X(market_buyers_pay, "trade") \
	X(market_sellers_get, "trade") \
	/* the national stockpile of the country trades like a pop or a factory */ \
	X(country_stockpile_buys, "trade") \
	X(country_stockpile_sells, "trade") \
	/* tariffs and gold */ \
	X(tariff_income, "tariff") \
	X(gold_income, "gold") \
	/* money created when gold is mined: the workers are paid GOLD_TO_WORKER_PAY_RATE * price per unit (the rules of Victoria 2) */ \
	X(gold_worker_pay, "gold") \
	/* the budget of the country */ \
	X(budget_administration, "budget") \
	X(budget_education, "budget") \
	X(budget_military, "budget") \
	X(budget_pensions, "subsidy") \
	X(budget_unemployment_subsidies, "subsidy") \
	X(budget_import_subsidies, "subsidy") \
	/* gross income handed to the pops (before the tax), the tax the pops pay, money the artisans lose, RGO revenue */ \
	X(gross_income_rgo, "income") \
	X(gross_income_government, "budget") \
	X(gross_income_other, "other") \
	X(pop_tax_paid, "tax") \
	X(rgo_revenue, "income") \
	X(sell_callbacks_rgo, "trade") \
	X(sell_callbacks_artisans, "trade") \
	X(sell_callbacks_factories, "trade") \
	X(sell_callbacks_countries, "trade") \
	X(buy_callbacks_money_spent, "trade") \
	/* what the pops get (after the tax) and spend */ \
	X(pop_rgo_owner_income, "income") \
	X(pop_rgo_worker_income, "income") \
	X(pop_factory_worker_income, "income") \
	X(pop_factory_owner_income, "income") \
	X(pop_unemployment_subsidies, "subsidy") \
	X(pop_pensions, "subsidy") \
	X(pop_government_salary_administration, "budget") \
	X(pop_government_salary_education, "budget") \
	X(pop_government_salary_military, "budget") \
	X(pop_event_and_decision_income, "other") \
	X(pop_loan_interest_payments, "credit") \
	X(pop_import_subsidies, "subsidy") \
	X(pop_artisanal_revenue, "income") \
	X(pop_life_needs_expense, "trade") \
	X(pop_everyday_needs_expense, "trade") \
	X(pop_luxury_needs_expense, "trade") \
	X(pop_artisan_inputs_expense, "trade") \
	X(pop_cash_taken, "credit") \
	/* factories */ \
	X(factory_revenue, "factory") \
	X(factory_spent, "factory") \
	X(factory_wages, "factory") \
	X(factory_dividends, "factory") \
	X(factory_starting_budget, "factory") \
	X(factory_budget_floor, "factory") \
	X(factory_subsidy, "subsidy") \
	X(factory_private_cover, "subsidy") \
	X(capitalist_fund_in, "factory") \
	X(capitalist_fund_out, "factory") \
	X(expansion_fund_spent, "factory") \
	/* the investment fund of a country pays the construction of an enterprise or a railroad */ \
	X(investment_fund_spent, "factory") \
	/* the other changes of the treasury of a country (add_cash / spend_cash) */ \
	X(country_other_credit, "other") \
	X(country_other_debit, "other")

		enum struct label_t : std::size_t {
#define OV_MONEY_ENUM(name, category) name,
			OV_MONEY_LABELS(OV_MONEY_ENUM)
#undef OV_MONEY_ENUM
			COUNT
		};
		static constexpr std::size_t LABEL_COUNT = static_cast<std::size_t>(label_t::COUNT);

		struct Totals {
			fixed_point_t pops, countries, factory_budgets, expansion_funds, capitalist_funds;
			fixed_point_t total() const {
				return pops + countries + factory_budgets + expansion_funds + capitalist_funds;
			}
		};

		// The ledger the hooks write to, nullptr when the ledger is off
		static inline MoneyLedger* active = nullptr;

		// The gross income of a pop, by the kind of the income (the name of the income function of the pop)
		static void record_gross_income(std::string_view income_name, const fixed_point_t amount) {
			if (active == nullptr) {
				return;
			}
			if (income_name.starts_with("rgo_")) {
				record(label_t::gross_income_rgo, amount);
			} else if (income_name.starts_with("government_") || income_name == "pensions" || income_name == "unemployment_subsidies") {
				record(label_t::gross_income_government, amount);
			} else {
				record(label_t::gross_income_other, amount);
			}
		}

		static void record(const label_t label, const fixed_point_t amount) {
			if (active != nullptr) {
				active->flows[static_cast<std::size_t>(label)].fetch_add(amount.get_raw_value(), std::memory_order_relaxed);
			}
		}

	private:
		std::array<std::atomic<std::int64_t>, LABEL_COUNT> flows {};
		std::array<std::int64_t, LABEL_COUNT> flow_totals {};
		std::vector<std::pair<std::string, std::int64_t>> phase_totals;
		Totals last_totals;
		std::int64_t days = 0;
		std::int64_t period_start_total = 0;
		std::int64_t start_total = 0;
		bool started = false;

	public:
		~MoneyLedger() {
			disable();
		}

		static Totals measure(InstanceManager& instance_manager);

		void enable(InstanceManager& instance_manager);
		void disable();
		bool is_enabled() const {
			return active == this;
		}
		// Called by the tick: the start of a day, and the end of each phase
		void begin_day(InstanceManager& instance_manager);
		void end_phase(InstanceManager& instance_manager, std::string const& phase);
		void end_day(InstanceManager& instance_manager);

		// For the tests: the total of a label since the ledger was switched on (raw fixed point, credits positive)
		std::int64_t get_flow_total_raw(label_t label) const;
		// The sum of the changes of the holders by the hooks, and the change of the money that was measured (raw)
		std::int64_t get_holder_sum_raw() const;
		std::int64_t get_measured_change_raw(InstanceManager& instance_manager) const;
		std::int64_t get_days() const {
			return days;
		}

		// Text of the report: money by holder, change per phase and per label since the last report, yearly growth
		std::string report(InstanceManager& instance_manager);
	};
}

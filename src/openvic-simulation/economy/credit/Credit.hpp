#pragma once

#include <cstdint>
#include <memory>
#include <span>
#include <unordered_map>

#include "openvic-simulation/core/memory/Vector.hpp"
#include "openvic-simulation/core/object/Date.hpp"
#include "openvic-simulation/core/object/FixedPoint.hpp"
#include "openvic-simulation/core/object/Timespan.hpp"

// The credit system. The formulas and the rules are described in docs/CREDITS.md, the numbers are in the group ov_credit of
// common/openvic_defines.lua (defines/OpenVicDefines.hpp, OV_CREDIT_DEFINES).
namespace OpenVic {
	struct CountryInstance;
	struct InstanceManager;
	struct OpenVicCreditDefines;
	struct State;

	// A credit. A lender first offers it (status OFFERED, the money is held by the credit), somebody takes it (ACTIVE: the money
	// is paid out and the schedule starts), it ends when it is paid back, written off or when the lender takes the offer back
	// (CLOSED).
	// Internal credits are offered by a country to the capitalists of its regions, which take them when they need money for a
	// factory. External credits are offered by a country to all countries or to chosen ones.
	struct Loan {
		enum struct kind_t : std::uint8_t { INTERNAL, EXTERNAL };
		// SHORT: the money can only be spent on constructions of at most SHORT_MAX_CONSTRUCTION_DAYS, MEDIUM and LONG: on anything
		// Lengths and the delay of the payments of every term are in the defines.
		enum struct term_t : std::uint32_t { SHORT = 0, MEDIUM = 1, LONG = 2 };
		enum struct status_t : std::uint8_t { OFFERED, ACTIVE, CLOSED };
		// How a credit ended
		enum struct end_t : std::uint8_t { NONE, PAID, WRITTEN_OFF, DEFAULTED, WITHDRAWN, EXPIRED };
		// What the money of a credit is spent on
		enum struct purpose_t : std::uint8_t { CONSTRUCTION, LOSS_COVER };

		std::uint32_t id = 0;
		kind_t kind = kind_t::EXTERNAL;
		status_t status = status_t::OFFERED;
		end_t end = end_t::NONE;
		CountryInstance* lender = nullptr;
		// The country that took an external credit (nullptr while it is offered), for internal credits the country of the state
		CountryInstance* borrower = nullptr;
		// The state whose capitalists took an internal credit (nullptr while it is offered)
		State* state = nullptr;
		// Who may take the credit: empty = all countries (external) / all regions of the lender (internal)
		memory::vector<CountryInstance*> target_countries;
		memory::vector<State*> target_states;

		fixed_point_t principal;
		// The principal that is still owed, and the interest that is still to be paid (the part of it that is already earned
		// during the delay and the part that will be earned later)
		fixed_point_t principal_left;
		fixed_point_t interest_left;
		// The interest earned during the delay before a change of the principal (a partial early repayment)
		fixed_point_t interest_locked;
		// The part of an internal credit the capitalists have not spent yet
		fixed_point_t unspent;
		// Yearly interest (0.05 = 5%), set once when the credit is offered
		fixed_point_t annual_rate;
		fixed_point_t interest_paid;
		fixed_point_t principal_paid;

		term_t term = term_t::SHORT;
		std::int32_t term_days = 0;
		std::int32_t delay_days = 0;
		// Number of monthly payments that are still to be made
		std::int32_t installments_left = 0;

		Date issue_date;
		Date taken_date;
		// The first payment is not due before this date
		Date payments_start;
		// What is not paid back by this date is a default
		Date due_date;
		// The date from which the principal earns the interest that is not yet in interest_left (changes with early repayments)
		Date accrual_from;
		bool has_expiry = false;
		Date expiry_date;

		fixed_point_t get_remaining() const {
			return principal_left + interest_left;
		}
		bool is_taken() const {
			return status == status_t::ACTIVE;
		}
		bool may_be_taken_by(CountryInstance const& country) const;

		// Short credits can only be spent on constructions that take a year or less, the others on anything
		bool can_fund(purpose_t purpose, std::int32_t construction_days, std::int32_t short_max_days) const;

		// The interest of a credit over its whole life (docs/CREDITS.md): the principal earns the rate during the delay, and
		// afterwards it is paid back in equal parts, so on average half of it is owed during the payments.
		static fixed_point_t calculate_total_interest(
			fixed_point_t principal, fixed_point_t annual_rate, std::int32_t delay_days, std::int32_t repayment_days
		);
		static std::int32_t calculate_installments(std::int32_t term_days, std::int32_t delay_days);

		// The schedule starts: the money is paid out on this date
		void start_schedule(Date today);
		// What is due in this month's installment (zero before the payments start)
		fixed_point_t get_installment_due(Date today) const;
		// Records a payment of the installment (the interest and the principal in the proportion of the schedule). The month
		// counts whether the payment was in full or not: what is missing is spread over the payments that are left.
		void apply_installment_payment(fixed_point_t due, fixed_point_t paid);
		// What it takes to pay everything back today (the principal and the interest earned during the delay)
		fixed_point_t get_full_repayment_cost(Date today) const;
		// Pays (a part of) the principal back early, the schedule of the rest is calculated again. Returns what was paid.
		fixed_point_t apply_early_repayment(Date today, fixed_point_t amount);
	};

	struct CreditManager {
		static constexpr std::int32_t DAYS_PER_INSTALLMENT = 30;

		// The most a country can owe (to others) before nobody lends to it
		static fixed_point_t get_debt_limit(OpenVicCreditDefines const& defines, CountryInstance const& country);

	private:
		memory::vector<std::unique_ptr<Loan>> loans;
		std::uint32_t next_loan_id = 1;
		OpenVicCreditDefines const* defines = nullptr;
		// The countries and regions the player has chosen in the tables, until the credit is offered
		std::unordered_map<CountryInstance const*, memory::vector<CountryInstance*>> staged_countries;
		std::unordered_map<CountryInstance const*, memory::vector<State*>> staged_states;

		Loan& add_loan(Loan&& loan);
		void close(Loan& loan, Loan::end_t how);
		// The payment goes from the borrower to the lender: from the treasury of a country, from the savings of capitalists
		fixed_point_t pay(Loan& loan, fixed_point_t amount);
		void process_monthly(InstanceManager& instance_manager, Loan& loan);
		void handle_due(InstanceManager& instance_manager, Loan& loan);
		void seize(InstanceManager& instance_manager, Loan& loan);
		void check_parties(InstanceManager& instance_manager, Loan& loan);
		void activate(InstanceManager& instance_manager, Loan& loan, CountryInstance* borrower, State* state);
		void expire_offers(InstanceManager& instance_manager);
		void run_ai(InstanceManager& instance_manager);
		void run_ai_lending(InstanceManager& instance_manager);
		void run_ai_borrowing(InstanceManager& instance_manager);
		void run_capitalist_investment(InstanceManager& instance_manager);
		// Capitalists of a state that cannot pay for a project take the cheapest internal credit that makes it possible
		bool take_internal_offer_for(
			InstanceManager& instance_manager, State& state, fixed_point_t cost, std::int32_t construction_days
		);
		std::int32_t count_open_offers(CountryInstance const& lender, Loan::kind_t kind) const;
		bool lender_accepts(
			InstanceManager const& instance_manager, Loan const& loan, CountryInstance const& borrower
		) const;

	public:
		// Called when the instance is created, the numbers come from the defines of the game
		void set_defines(OpenVicCreditDefines const& new_defines) {
			defines = &new_defines;
		}
		OpenVicCreditDefines const& get_defines() const {
			return *defines;
		}

		std::span<const std::unique_ptr<Loan>> get_loans() const {
			return loans;
		}
		Loan const* get_loan_by_id(std::uint32_t id) const;

		// The length of a term and the delay of its payments, from the defines
		std::int32_t get_term_days(Loan::term_t term) const;
		std::int32_t get_delay_days(Loan::term_t term) const;

		// What a country owes to other countries (external credits it took)
		fixed_point_t get_total_debt(CountryInstance const& borrower) const;
		// What the country has to pay in a month for its external credits
		fixed_point_t get_monthly_payments(CountryInstance const& borrower, Date today) const;

		// The player chooses countries / regions in a table, then offers the credit to them
		void stage_target_country(CountryInstance const& lender, CountryInstance& target, bool selected);
		void stage_target_state(CountryInstance const& lender, State& target, bool selected);
		void clear_staged(CountryInstance const& lender);
		memory::vector<CountryInstance*> const& get_staged_countries(CountryInstance const& lender) const;
		memory::vector<State*> const& get_staged_states(CountryInstance const& lender) const;

		// A country offers money to the capitalists of its regions (all of them or the staged ones). The money is held by
		// the credit until somebody takes it or the offer is withdrawn.
		Loan* offer_internal_credit(
			InstanceManager& instance_manager, CountryInstance& lender, fixed_point_t amount, fixed_point_t rate, Loan::term_t term,
			bool all_regions, bool as_ai = false
		);
		// A country offers money to all countries or to the staged ones. If it names countries, nobody else can take it.
		Loan* offer_external_credit(
			InstanceManager& instance_manager, CountryInstance& lender, fixed_point_t amount, fixed_point_t rate, Loan::term_t term,
			bool all_countries, bool as_ai = false
		);
		// The lender takes the offer back, the held money returns to its treasury
		bool withdraw_offer(InstanceManager& instance_manager, CountryInstance& lender, std::uint32_t id);
		// A country takes an offered external credit
		bool take_credit(InstanceManager& instance_manager, CountryInstance& borrower, std::uint32_t id);
		// The capitalists of a region take an offered internal credit (they do it themselves when they need money for a factory,
		// this is for the cases where the choice is made by a script or a test)
		bool take_internal_credit(InstanceManager& instance_manager, std::uint32_t id, State& state);
		// Offers that country can take now (external, addressed to it or to everybody)
		memory::vector<Loan const*> get_available_offers(InstanceManager const& instance_manager, CountryInstance const& borrower) const;

		// Pays back (a part of) the principal of a credit early, from the money of the borrower. Returns what was paid.
		fixed_point_t repay_loan(InstanceManager& instance_manager, Loan& loan, fixed_point_t amount);
		bool repay_loan_by_id(InstanceManager& instance_manager, CountryInstance& payer, std::uint32_t id, fixed_point_t amount);

		// Money the capitalists of the state can spend: their savings and the unspent parts of the credits that allow it
		fixed_point_t get_capitalist_money(State const& state, Loan::purpose_t purpose, std::int32_t construction_days) const;
		fixed_point_t take_capitalist_money(
			State& state, fixed_point_t amount, Loan::purpose_t purpose, std::int32_t construction_days
		);

		void tick(InstanceManager& instance_manager);
	};
}

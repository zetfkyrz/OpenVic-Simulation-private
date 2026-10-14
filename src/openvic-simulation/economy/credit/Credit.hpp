#pragma once

#include <cstdint>
#include <memory>
#include <span>
#include <unordered_map>

#include "openvic-simulation/core/memory/Vector.hpp"
#include "openvic-simulation/core/object/Date.hpp"
#include "openvic-simulation/core/object/FixedPoint.hpp"
#include "openvic-simulation/core/object/Timespan.hpp"

namespace OpenVic {
	struct CountryInstance;
	struct InstanceManager;
	struct State;

	// A loan. Internal loans are given by the state (the central bank of a country) to the capitalists of one of its
	// states, to build factories and to keep them running at a loss. External loans are given by one country to another.
	struct Loan {
		enum struct kind_t : std::uint8_t { INTERNAL, EXTERNAL };
		// Length of the loan: 5, 10 or 20 years
		enum struct term_t : std::uint32_t { SHORT = 0, MEDIUM = 1, LONG = 2 };
		// What happens with a debt that is not paid when the loan is due
		enum struct maturity_policy_t : std::uint32_t {
			FORGIVE = 0, // the debt is written off
			SOFT = 1,    // the money of the capitalists is taken, the factories are not touched
			HARD = 2     // the money first, then the shares of the capitalists in the factories at their value
		};
		// What the money of a loan is spent on
		enum struct purpose_t : std::uint8_t { CONSTRUCTION, LOSS_COVER };

		std::uint32_t id = 0;
		kind_t kind = kind_t::EXTERNAL;
		CountryInstance* lender = nullptr;
		// The borrowing country, for internal loans the country of the state
		CountryInstance* borrower = nullptr;
		// The state whose capitalists borrowed the money (internal loans)
		State* state = nullptr;

		fixed_point_t principal;
		// What is still owed (the principal left plus the interest that could not be paid)
		fixed_point_t remaining;
		// The part of an internal loan the capitalists have not spent yet
		fixed_point_t unspent;
		// Yearly interest as a fraction (0.05 = 5%); the player may set 0%, AI countries never go below 1%
		fixed_point_t annual_rate;
		fixed_point_t interest_paid;

		term_t term = term_t::SHORT;
		maturity_policy_t policy = maturity_policy_t::SOFT;
		Date start_date;
		// Nothing has to be paid back before this date (internal loans)
		Date grace_end;
		Date due_date;
		bool active = true;
		// An external loan taken by a country that pays itself back every month as far as the cash allows
		bool auto_repay = false;

		static Timespan get_term_length(term_t term);
		static Timespan get_grace_length(term_t term);

		// Short loans can only be spent on constructions that take a year or less, the others on anything
		bool can_fund(purpose_t purpose, std::int32_t construction_days) const;
	};

	struct ExternalOffer {
		CountryInstance* lender = nullptr;
		fixed_point_t max_amount;
		fixed_point_t rate;
	};

	// An AI country that wants an alliance with a human country that offered it money
	struct AllianceRequest {
		CountryInstance* from = nullptr;
		CountryInstance* to = nullptr;
		Date date;
	};

	struct CreditManager {
		// Who gets a loan offered to a group of countries
		enum struct target_group_t : std::uint32_t {
			ALL = 0,      // every country
			ALLIES = 1,   // countries in an alliance with the lender
			FRIENDS = 2,  // countries whose relations with the lender are above 100
			SELECTED = 3  // the countries chosen in the table (stage_loan_target)
		};
		static constexpr std::int32_t FRIEND_RELATIONS = 100;

		// Lowest rate at all (the player can lend and borrow for free)
		static fixed_point_t get_min_rate() {
			return fixed_point_t::_0;
		}
		// AI countries never lend for less than this
		static fixed_point_t get_ai_min_rate() {
			return fixed_point_t(1) / 100;
		}
		static fixed_point_t get_max_rate() {
			return fixed_point_t(1) / 2;
		}
		static fixed_point_t clamp_rate(fixed_point_t rate);
		// The most a country can owe (to others) before nobody lends to it
		static fixed_point_t get_debt_limit(CountryInstance const& country);

	private:
		memory::vector<std::unique_ptr<Loan>> loans;
		std::uint32_t next_loan_id = 1;
		// Month (year * 12 + month) of the last internal loan given by each country: at most one per month
		std::unordered_map<CountryInstance const*, std::int32_t> last_internal_loan_month;

		void monthly_interest_and_repayment(InstanceManager& instance_manager, Loan& loan);
		void handle_maturity(InstanceManager& instance_manager, Loan& loan);
		void seize(InstanceManager& instance_manager, Loan& loan, bool hard);
		void run_ai(InstanceManager& instance_manager);
		void run_capitalist_investment(InstanceManager& instance_manager);

		Loan& add_loan(Loan&& loan);
		memory::vector<AllianceRequest> alliance_requests;
		std::unordered_map<CountryInstance const*, memory::vector<CountryInstance*>> staged_targets;

		// How an AI country answers an offer of a loan: accepts, or (if it needs the money) tries to get closer to the lender
		bool ai_accepts_loan(
			InstanceManager& instance_manager, CountryInstance& borrower, CountryInstance& lender, fixed_point_t amount,
			fixed_point_t rate, bool allow_reactions
		);

	public:
		std::span<const std::unique_ptr<Loan>> get_loans() const {
			return loans;
		}
		Loan const* get_loan_by_id(std::uint32_t id) const;

		fixed_point_t get_total_debt(CountryInstance const& borrower) const;
		fixed_point_t get_monthly_interest(CountryInstance const& borrower) const;

		// Offers of the AI countries that have money to lend
		memory::vector<ExternalOffer> get_offers(InstanceManager const& instance_manager, CountryInstance const& borrower) const;

		// A country takes money from the lender (the terms are the lender's offer: its external rate)
		bool take_external_loan(
			InstanceManager& instance_manager, CountryInstance& borrower, CountryInstance& lender, fixed_point_t amount,
			Loan::term_t term
		);
		// The player offers money to another country, an AI country accepts if the loan is good for it
		bool give_external_loan(
			InstanceManager& instance_manager, CountryInstance& lender, CountryInstance& borrower, fixed_point_t amount,
			fixed_point_t rate, Loan::term_t term
		);
		// The player offers the same loan to a group of countries, returns how many accepted
		std::size_t give_external_loans_to_group(
			InstanceManager& instance_manager, CountryInstance& lender, target_group_t group, fixed_point_t amount_each,
			fixed_point_t rate, Loan::term_t term
		);
		// Adds / removes a country in the list of the countries chosen for target_group_t::SELECTED
		void stage_loan_target(CountryInstance const& lender, CountryInstance& target, bool selected);
		memory::vector<CountryInstance*> const& get_staged_targets(CountryInstance const& lender) const;
		std::span<const AllianceRequest> get_alliance_requests() const {
			return alliance_requests;
		}
		bool answer_alliance_request(InstanceManager& instance_manager, CountryInstance& to, CountryInstance& from, bool accept);
		// The state lends to the capitalists of one of its states, at most once a month for a country
		bool give_internal_loan(
			InstanceManager& instance_manager, CountryInstance& lender, State& state, fixed_point_t amount, fixed_point_t rate,
			Loan::term_t term, Loan::maturity_policy_t policy
		);
		// Pays back (a part of) a loan from the money of the borrower, returns the amount paid
		fixed_point_t repay_loan(InstanceManager& instance_manager, Loan& loan, fixed_point_t amount);
		bool repay_loan_by_id(InstanceManager& instance_manager, CountryInstance& payer, std::uint32_t id, fixed_point_t amount);
		bool set_loan_auto_repay(CountryInstance& payer, std::uint32_t id, bool auto_repay);

		// Money the capitalists of the state can spend: their savings and the unspent parts of the loans that allow it
		fixed_point_t get_capitalist_money(State const& state, Loan::purpose_t purpose, std::int32_t construction_days) const;
		fixed_point_t take_capitalist_money(
			State& state, fixed_point_t amount, Loan::purpose_t purpose, std::int32_t construction_days
		);

		void tick(InstanceManager& instance_manager);
	};
}

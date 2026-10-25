#pragma once

#include <cstdint>
#include <optional>
#include <utility>

#include "openvic-simulation/defines/OpenVicDefines.hpp"
#include "openvic-simulation/core/memory/Vector.hpp"
#include "openvic-simulation/core/object/Date.hpp"
#include "openvic-simulation/core/object/FixedPoint.hpp"
#include "openvic-simulation/economy/BuildingLevel.hpp"
#include "openvic-simulation/population/PopSize.hpp"
#include "openvic-simulation/types/TypedIndices.hpp"
#include "openvic-simulation/utility/Getters.hpp"

namespace OpenVic {
	struct BuildingType;
	struct BuyResult;
	struct CountryInstance;
	struct GoodDefinition;
	struct MarketInstance;
	struct Pop;
	struct ProductionType;
	struct SellResult;
	struct State;

	/* A factory in a state. Unlike the original game it is not a flat "workers in, goods out" building, it is a small
	 * company with its own budget that adapts to the market:
	 * - it buys its inputs on the market with its own money and keeps a small stockpile of them,
	 * - its efficiency grows while it makes a profit and decays while it makes a loss,
	 * - it throttles production when it is not profitable and recovers when it is,
	 * - profit is shared between workers (wages), the owners (capitalists) and the factory budget,
	 * - profitable factories with enough budget are expanded by their owners without any player action,
	 * - factories that stay unprofitable for a long time close down.
	 *
	 * All market callbacks only write to fields that belong to a single order, the results are settled in the next tick
	 * as callbacks can be executed in parallel (one thread per good). */
	struct FactoryInstance {
		struct Input {
			GoodDefinition const& good;
			const fixed_point_t quantity_per_level;
			fixed_point_t stockpile;
			// Written by the market callback, settled at the start of the next tick
			fixed_point_t bought_today;
			fixed_point_t spent_today;

			Input(GoodDefinition const& new_good, const fixed_point_t new_quantity_per_level);
		};

		struct Worker {
			Pop* pop;
			pop_size_t size;
		};

		// The parameters of the factories are in the group ov_economy of common/openvic_defines.lua; the pointer is set by
		// the InstanceManager (the defines belong to the definitions and outlive all instances)
		static inline OpenVicEconomyDefines const* defines = nullptr;
		static void set_defines(OpenVicEconomyDefines const& new_defines) {
			defines = &new_defines;
		}
		static size_t get_max_factories_per_state() {
			return static_cast<size_t>(defines->max_factories_per_state);
		}

	private:
		building_level_t PROPERTY(level);
		bool PROPERTY_CUSTOM_PREFIX(open, is, true);
		std::optional<Date> PROPERTY(construction_start);
		std::optional<Date> PROPERTY(construction_end);

		fixed_point_t PROPERTY(efficiency);
		fixed_point_t PROPERTY(throttle);
		fixed_point_t PROPERTY(budget);
		// Part of the profit saved for the expansion of the factory, the factory expands when it covers the cost
		fixed_point_t PROPERTY(expansion_fund);
		// Share of the factory owned by the state (the rest belongs to the capitalists), 0..1
		fixed_point_t PROPERTY_RW(state_share);
		// Whether the factory was built by the state (otherwise by capitalists / the game setup)
		bool PROPERTY_RW_CUSTOM_NAME(built_by_state, was_built_by_state, set_built_by_state, false);
		// Hiring order inside a state: factories with a higher priority hire first (0 low, 1 normal, 2 high)
		std::uint32_t PROPERTY_RW(priority, 1);
		// A subsidised factory keeps running at a loss: its own budget is used first, and the state treasury pays only
		// the part of the purchases the budget cannot cover
		bool PROPERTY_CUSTOM_PREFIX(subsidised, is, false);
		fixed_point_t subsidy_today;
		fixed_point_t PROPERTY(subsidy_yesterday);
		// Losses covered by the capitalists of the state
		fixed_point_t private_cover_today;
		fixed_point_t PROPERTY(private_cover_yesterday);
		// Money the state paid this month for the capitalists in the hard anti-crisis mode: at the start of the next month
		// the state takes over the share of the factory worth this money
		fixed_point_t PROPERTY(crisis_covered);
		// Whether the factory has received its founding capital, which factories added by the game setup have not yet
		bool is_funded = false;
		// Country that collects the tariff on the inputs the factory imports, set when the orders are placed
		CountryInstance* tariff_collector = nullptr;
		// Construction ordered by the player is paid with money (the building type's cost) and with the goods of the
		// national stockpile, which are used up day by day while it is built. The pace accumulates the construction
		// spending slider: a day of construction is only worked when it reaches 1 and the goods of the day are there,
		// otherwise the end of the construction moves one day back.
		bool construction_uses_stockpile = false;
		fixed_point_t construction_pace;
		int32_t construction_total_days = 1;

		memory::vector<Input> SPAN_PROPERTY(inputs);
		memory::vector<Worker> SPAN_PROPERTY(workers);
		pop_size_t PROPERTY(employee_count, 0);
		pop_size_t PROPERTY(max_employee_count, 0);

		// Written by the sell callback, settled at the start of the next tick
		fixed_point_t revenue_today;
		fixed_point_t sold_quantity_today;
		// Values of the production of the previous tick, which are settled together with the orders results
		fixed_point_t produced_quantity_pending;
		fixed_point_t consumed_value_pending;

		fixed_point_t PROPERTY(output_quantity_yesterday);
		fixed_point_t PROPERTY(sold_quantity_yesterday);
		fixed_point_t PROPERTY(revenue_yesterday);
		fixed_point_t PROPERTY(input_cost_yesterday);
		fixed_point_t PROPERTY(wages_yesterday);
		fixed_point_t PROPERTY(owner_income_yesterday);
		fixed_point_t PROPERTY(state_income_yesterday);
		fixed_point_t PROPERTY(profit_yesterday);
		// Moving average of the daily profit (about the last month), the base of the value of the factory
		fixed_point_t PROPERTY(average_profit);
		uint32_t PROPERTY(profitable_days, 0);
		uint32_t PROPERTY(unprofitable_days, 0);

		static void after_buy(void* actor, BuyResult const& buy_result);
		static void after_sell(void* actor, SellResult const& sell_result, memory::vector<fixed_point_t>& reusable_vector);

		void settle(State& state, MarketInstance const& market_instance);
		void advance_construction(State& state, const Date today, MarketInstance const& market_instance);
		void try_auto_expansion(const Date today, MarketInstance const& market_instance);
		// Pays the dividend out of the budget to the capitalists and the state according to the shares, adds what each got
		void pay_dividend(State& state, fixed_point_t amount, fixed_point_t& capitalists_income, fixed_point_t& state_income);
		void hire(State& state);
		// Gives the factory a starting budget of 5% of the cost of the levels that were just built
		void grant_starting_budget(MarketInstance const& market_instance, building_level_t levels);

	public:
		BuildingType const& building_type;
		ProductionType const& production_type;

		FactoryInstance(BuildingType const& new_building_type, ProductionType const& new_production_type, building_level_t new_level);
		FactoryInstance(FactoryInstance&&) = delete;
		FactoryInstance(FactoryInstance const&) = delete;

		[[nodiscard]] bool is_under_construction() const {
			return construction_end.has_value();
		}
		// Fraction of the construction that is done, 0 if nothing is being built
		[[nodiscard]] fixed_point_t get_construction_progress(const Date today) const;
		[[nodiscard]] bool can_be_expanded() const;

		// Cost of building or expanding by one level at the current market prices
		[[nodiscard]] static fixed_point_t calculate_construction_cost(
			BuildingType const& building_type, MarketInstance const& market_instance
		);
		[[nodiscard]] static Timespan calculate_construction_time(BuildingType const& building_type);

		// With uses_stockpile the goods of the building type are taken from the national stockpile during the construction
		void start_construction(const Date today, bool uses_stockpile = false);
		[[nodiscard]] bool is_construction_using_stockpile() const {
			return construction_uses_stockpile && construction_end.has_value();
		}
		// Quantity of the good used up on a day when the construction is worked
		[[nodiscard]] fixed_point_t get_daily_construction_goods(GoodDefinition const& good) const;
		void set_open(const bool new_open);

		// What the whole factory is worth: its yearly profit times the payback years of the defines, but not less than a share of the cost of
		// building it again
		[[nodiscard]] fixed_point_t calculate_value(MarketInstance const& market_instance) const;
		// The state buys (state_buys) the given share (0..1) of the factory from the capitalists or sells it to them.
		// The state pays out of / receives into its treasury, the capitalists use / fill the savings fund of the state.
		// Returns false when nothing could be traded.
		bool trade_share(
			State& state, CountryInstance& country, MarketInstance const& market_instance, fixed_point_t share,
			bool state_buys
		);
		// The state really pays (a part of) the losses of the factory: it is subsidised and the subsidy mode says that the
		// state pays for it: the mode "all", or the state owns a share of the factory, or (anti-crisis mode) the capitalists of
		// the state have run out of money. A subsidised private factory in the mode "by share" is not helped by the state and
		// works like any other factory.
		[[nodiscard]] bool receives_state_subsidy(CountryInstance const* owner, State const& state) const;

		// The share the state would take over for the money it covered for the capitalists, in whole percents (rounded up,
		// never more than the share of the capitalists), 0 if there is nothing to take
		[[nodiscard]] std::int32_t get_pending_takeover_percent(MarketInstance const& market_instance) const;
		// Takes the share over (monthly, hard anti-crisis mode): the share of the state grows, the shares stay within 100%
		void apply_crisis_takeover(MarketInstance const& market_instance);

		// What a trade of a share of the factory would be like (the same rules as trade_share)
		struct TradeQuote {
			enum struct reason_t : std::uint8_t { OK, NOT_BUILT, NOTHING_TO_TRADE, NO_CASH, NO_PROFIT, NO_CAPITAL };
			bool possible = false;
			reason_t reason = reason_t::OK;
			// The share that would be traded (the capitalists may only afford a part of what was asked) and its price
			fixed_point_t share;
			fixed_point_t price;
		};
		[[nodiscard]] TradeQuote quote_trade(
			State const& state, CountryInstance const& country, MarketInstance const& market_instance, fixed_point_t share,
			bool state_buys
		) const;

		void set_subsidised(const bool new_subsidised) {
			subsidised = new_subsidised;
		}

		void tick(State& state, MarketInstance& market_instance, const Date today);
	};
}

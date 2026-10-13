#pragma once

#include <cstdint>
#include <optional>
#include <utility>

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

		static constexpr size_t MAX_FACTORIES_PER_STATE = 8;

	private:
		building_level_t PROPERTY(level);
		bool PROPERTY_CUSTOM_PREFIX(open, is, true);
		std::optional<Date> PROPERTY(construction_start);
		std::optional<Date> PROPERTY(construction_end);

		fixed_point_t PROPERTY(efficiency);
		fixed_point_t PROPERTY(throttle);
		fixed_point_t PROPERTY(budget);
		// Whether the factory has received its founding capital, which factories added by the game setup have not yet
		bool is_funded = false;

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
		fixed_point_t PROPERTY(profit_yesterday);
		uint32_t PROPERTY(profitable_days, 0);
		uint32_t PROPERTY(unprofitable_days, 0);

		static void after_buy(void* actor, BuyResult const& buy_result);
		static void after_sell(void* actor, SellResult const& sell_result, memory::vector<fixed_point_t>& reusable_vector);

		void settle(State& state, MarketInstance const& market_instance);
		void advance_construction(const Date today, MarketInstance const& market_instance);
		void try_private_expansion(const Date today, MarketInstance const& market_instance);
		void hire(State& state);
		void top_up_budget(MarketInstance const& market_instance);

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

		void start_construction(const Date today);
		void set_open(const bool new_open);

		void tick(State& state, MarketInstance& market_instance, const Date today);
	};
}

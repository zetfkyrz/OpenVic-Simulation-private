#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "openvic-simulation/core/memory/Vector.hpp"
#include "openvic-simulation/core/object/FixedPoint.hpp"
#include "openvic-simulation/core/stl/containers/TypedSpan.hpp"
#include "openvic-simulation/core/thread/SpinMutex.hpp"
#include "openvic-simulation/economy/trading/BuyUpToOrder.hpp"
#include "openvic-simulation/economy/trading/MarketSellOrder.hpp"
#include "openvic-simulation/types/TypedIndices.hpp"
#include "openvic-simulation/types/ValueHistory.hpp"
#include "openvic-simulation/utility/Getters.hpp"

namespace OpenVic {
	struct GameRulesManager;
	struct GoodDefinition;
	struct TradeNetwork;

	struct GoodMarket {
	private:
		static constexpr int32_t exponential_price_change_shift = 7;
		spin_mutex buy_lock;
		spin_mutex sell_lock;
		GameRulesManager const& game_rules_manager;
		fixed_point_t absolute_maximum_price;
		fixed_point_t absolute_minimum_price;

		//only used during day tick (from actors placing order until execute_orders())
		memory::vector<GoodBuyUpToOrder> buy_up_to_orders;
		memory::vector<GoodMarketSellOrder> market_sell_orders;

		// World trade (docs/TRADE.md): when the network is on, the day is cleared in three steps. First the goods that the countries cannot
		// use themselves are planned along the routes (cheapest first, within the capacity of the routes), the buyers of a country can only
		// buy what its sellers offer plus what the routes bring. Then the day is cleared like before. At last it is worked out how much of
		// every order was sold at home and abroad, and the freight of the sellers is taken from their money.
		TradeNetwork* trade_network = nullptr;
		struct route_flow_t {
			std::uint32_t from;
			std::uint32_t to;
			fixed_point_t quantity;
			fixed_point_t cost;
			std::uint8_t mode;
		};
		// Scratch arrays of the routed day (by node: the countries and the outside), kept to avoid allocations
		std::vector<fixed_point_t> node_supply, node_desire, node_bought, node_cap, node_inflow, node_dom, node_exports, node_export_cost,
			node_fee_domestic, node_fee_export, node_land_exports, node_sea_exports, node_imports, node_covered;
		std::vector<route_flow_t> route_flows;
		std::vector<fixed_point_t> effective_max_quantity;
		// The part of the offers (and of the wishes) that cannot reach each other because the routes are missing or full
		fixed_point_t cut_off_quantity;
		// Plans the exports of the countries along the routes; fills route_flows, node_inflow and node_cap. Returns the part of the offers that is not cut off from the buyers
		// (the offers that the home buyers can use, what the routes carry and the glut that nobody wants)
		fixed_point_t plan_routes();
		// Works out how much of every order was sold at home and abroad, pays the sellers (minus the freight) and sends the freight to the
		// treasuries of their countries
		void attribute_routed_sales(const fixed_point_t new_price, memory::vector<fixed_point_t>& reusable_vector);

		void execute_buy_orders(
			const fixed_point_t new_price,
			TypedSpan<country_index_t, const fixed_point_t> actual_bought_per_country,
			TypedSpan<country_index_t, const fixed_point_t> supply_per_country,
			std::span<const fixed_point_t> quantity_bought_per_order
		);

	protected:
		bool PROPERTY_ACCESS(is_available, protected);

	private:
		fixed_point_t PROPERTY(price);
		fixed_point_t PROPERTY(price_inverse);
		fixed_point_t PROPERTY(price_change_yesterday);
		fixed_point_t PROPERTY(max_next_price);
		fixed_point_t PROPERTY(min_next_price);
		fixed_point_t PROPERTY(total_demand_yesterday);
		fixed_point_t PROPERTY(total_supply_yesterday);
		fixed_point_t PROPERTY(quantity_traded_yesterday);
		ValueHistory<fixed_point_t> PROPERTY(price_history);

		void update_next_price_limits();
	public:
		GoodDefinition const& good_definition;

		void set_trade_network(TradeNetwork* new_trade_network) {
			trade_network = new_trade_network;
		}

		GoodMarket(GameRulesManager const& new_game_rules_manager, GoodDefinition const& new_good_definition);
		GoodMarket(GoodMarket const&) = delete;
		GoodMarket& operator=(GoodMarket const&) = delete;
		GoodMarket(GoodMarket&&) = delete;
		GoodMarket& operator=(GoodMarket&&) = delete;

		//thread safe
		void add_buy_up_to_order(GoodBuyUpToOrder&& buy_up_to_order);
		void add_market_sell_order(GoodMarketSellOrder&& market_sell_order);

		//not thread safe
		static constexpr size_t VECTORS_FOR_EXECUTE_ORDERS = 2;
		void execute_orders(
			TypedSpan<country_index_t, fixed_point_t> reusable_country_map_0,
			TypedSpan<country_index_t, fixed_point_t> reusable_country_map_1,
			std::span<
				memory::vector<fixed_point_t>,
				VECTORS_FOR_EXECUTE_ORDERS
			> reusable_vectors
		);
		void on_use_exponential_price_changes_changed();
		void record_price_history();
	};
}
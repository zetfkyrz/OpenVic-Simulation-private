#pragma once

#include "openvic-simulation/core/memory/Vector.hpp"
#include "openvic-simulation/core/object/FixedPoint.hpp"
#include "openvic-simulation/core/thread/SpinMutex.hpp"
#include "openvic-simulation/economy/GoodInstance.hpp"
#include "openvic-simulation/economy/trading/MarketSellOrder.hpp"
#include "openvic-simulation/types/TypedIndices.hpp"

namespace OpenVic {
	struct BuyUpToOrder;
	struct CountryDefines;
	struct GoodInstance;
	struct GoodInstanceManager;
	struct MarketSellOrder;
	struct ThreadPool;

	struct MarketInstance {
	private:
		ThreadPool& thread_pool;
		CountryDefines const& country_defines;
		GoodInstanceManager& good_instance_manager;

		// Gold is not traded. Its orders wait here (the RGOs place them while the provinces are being processed at the same time) until
		// the market has run, and then the workers and owners are paid in one thread. The owners are pops of other provinces, whose own
		// tick reads their money at the same time, so a payment at once made the orders of the day depend on the speed of the threads.
		spin_mutex gold_orders_lock;
		memory::vector<MarketSellOrder> gold_sell_orders;
		void pay_gold_orders();
	public:
		MarketInstance(
			ThreadPool& new_thread_pool,
			CountryDefines const& new_country_defines,
			GoodInstanceManager& new_good_instance_manager
		) : thread_pool { new_thread_pool },
			country_defines { new_country_defines },
			good_instance_manager { new_good_instance_manager } {}

		// Inline: every pop asks these for every good of its needs every day
		bool get_is_available(const good_index_t good_index) const {
			return good_instance_manager.get_good_instance_by_index(good_index)->get_is_available();
		}
		fixed_point_t get_max_next_price(const good_index_t good_index) const {
			return good_instance_manager.get_good_instance_by_index(good_index)->get_max_next_price();
		}
		fixed_point_t get_min_next_price(const good_index_t good_index) const {
			return good_instance_manager.get_good_instance_by_index(good_index)->get_min_next_price();
		}
		fixed_point_t get_max_money_to_allocate_to_buy_quantity(const good_index_t good_index, const fixed_point_t quantity) const {
			//round up so money_to_spend >= max_next_price * max_quantity_to_buy;
			//always add epsilon as money_to_spend == max_next_price * max_quantity_to_buy is rare and this is cheaper for performance.
			return quantity * get_max_next_price(good_index) + fixed_point_t::epsilon;
		}
		GoodInstance const& get_good_instance(const good_index_t good_index) const {
			return *good_instance_manager.get_good_instance_by_index(good_index);
		}
		void place_buy_up_to_order(BuyUpToOrder&& buy_up_to_order);
		void place_market_sell_order(MarketSellOrder&& market_sell_order, memory::vector<fixed_point_t>& reusable_vector);
		void execute_orders();
		void record_price_history();
	};
}

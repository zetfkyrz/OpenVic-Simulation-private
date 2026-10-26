#include "openvic-simulation/misc/MoneyLedger.hpp"
#include "GoodMarket.hpp"

#include <algorithm>
#include "openvic-simulation/utility/Logger.hpp"
#include <mutex>

#include "openvic-simulation/core/object/FixedPoint.hpp"
#include "openvic-simulation/core/object/FixedPoint/Math.hpp"
#include "openvic-simulation/economy/GoodDefinition.hpp"
#include "openvic-simulation/economy/trading/BuyUpToOrder.hpp"
#include "openvic-simulation/economy/trading/MarketSellOrder.hpp"
#include "openvic-simulation/economy/trading/TradeNetwork.hpp"
#include "openvic-simulation/misc/GameRulesManager.hpp"

using namespace OpenVic;

static constexpr size_t MONTHS_OF_PRICE_HISTORY = 36;

GoodMarket::GoodMarket(GameRulesManager const& new_game_rules_manager, GoodDefinition const& new_good_definition)
  : game_rules_manager { new_game_rules_manager },
	good_definition { new_good_definition },
	price { new_good_definition.base_price },
	is_available { new_good_definition.is_available_from_start },
	price_history { MONTHS_OF_PRICE_HISTORY, new_good_definition.base_price }
	{
		on_use_exponential_price_changes_changed();
		update_next_price_limits();
	}

void GoodMarket::on_use_exponential_price_changes_changed() {
	if (game_rules_manager.get_use_exponential_price_changes()) {
		absolute_maximum_price = fixed_point_t::usable_max;
		absolute_minimum_price = fixed_point_t::epsilon << exponential_price_change_shift;
	} else {
		absolute_maximum_price = std::min(
			good_definition.base_price * 5,
			fixed_point_t::usable_max
		);
		absolute_minimum_price = std::max(
			good_definition.base_price * 22 / 100,
			fixed_point_t::epsilon
		);
	}
}

void GoodMarket::update_next_price_limits() {
	const fixed_point_t max_price_change = game_rules_manager.get_use_exponential_price_changes()
		? price >> exponential_price_change_shift
		: fixed_point_t::_0_01;

	max_next_price = std::min(
		absolute_maximum_price,
		price + max_price_change
	);
	min_next_price = std::max(
		absolute_minimum_price,
		price - max_price_change
	);

	price_inverse = fixed_point_t::_1 / price;
}

void GoodMarket::add_buy_up_to_order(GoodBuyUpToOrder&& buy_up_to_order) {
	const std::lock_guard<spin_mutex> lock_guard { buy_lock };
	buy_up_to_orders.push_back(std::move(buy_up_to_order));
}

void GoodMarket::add_market_sell_order(GoodMarketSellOrder&& market_sell_order) {
	const std::lock_guard<spin_mutex> lock_guard { sell_lock };
	market_sell_orders.push_back(std::move(market_sell_order));
}

void GoodMarket::execute_orders(
	TypedSpan<country_index_t, fixed_point_t> reusable_country_map_0,
	TypedSpan<country_index_t, fixed_point_t> reusable_country_map_1,
	std::span<
		memory::vector<fixed_point_t>,
		VECTORS_FOR_EXECUTE_ORDERS
	> reusable_vectors
) {
	if (!is_available) {
		//price remains the same
		price_change_yesterday
			= quantity_traded_yesterday
			= total_demand_yesterday
			= total_supply_yesterday
			= 0;

		for (GoodBuyUpToOrder const& buy_up_to_order : buy_up_to_orders) {
			buy_up_to_order.call_after_trade(BuyResult::no_purchase_result(good_definition.index));
		}
		buy_up_to_orders.clear();

		for (GoodMarketSellOrder const& market_sell_order : market_sell_orders) {
			market_sell_order.call_after_trade(SellResult::no_sales_result(good_definition.index), reusable_vectors[0]);
		}		
		market_sell_orders.clear();
		
		return;
	}

	fixed_point_t new_price;
	//MarketInstance ensured only orders with quantity > 0 are added.
	//So running total > 0 unless orders are empty.
	fixed_point_t demand_sum = 0,
		supply_sum = 0;
	if (market_sell_orders.empty()) {
		quantity_traded_yesterday = 0;
		fixed_point_t max_affordable_price = price;
		for (GoodBuyUpToOrder const& buy_up_to_order : buy_up_to_orders) {
			const fixed_point_t affordable_price = buy_up_to_order.get_affordable_price();
			if (affordable_price > max_affordable_price) {
				max_affordable_price = affordable_price;
			}

			demand_sum += buy_up_to_order.max_quantity;
			buy_up_to_order.call_after_trade(BuyResult::no_purchase_result(good_definition.index));
		}

		if (game_rules_manager.get_use_optimal_pricing()) {
			new_price = std::min(max_next_price, max_affordable_price);
		} else {
			//TODO use Victoria 2's square root mechanic, see https://github.com/OpenVicProject/OpenVic/issues/288
			if (demand_sum > 0) {
				new_price = max_next_price;
			} else {
				new_price = price;
			}
		}
	} else {
		// The routed day (see the header): the countries and the "outside" are the nodes of the trade network
		const bool routed = trade_network != nullptr && trade_network->is_enabled() && trade_network->is_ready();
		const std::size_t node_count = routed ? trade_network->get_node_count() : 0;
		const country_index_t outside_node = routed ? trade_network->get_outside_node() : country_index_t(0);
		const auto node_of = [outside_node](const std::optional<country_index_t> country_index_optional) -> country_index_t {
			return country_index_optional.has_value() ? country_index_optional.value() : outside_node;
		};
		if (routed) {
			for (std::vector<fixed_point_t>* array : {
				&node_supply, &node_desire, &node_bought, &node_cap, &node_inflow, &node_dom, &node_exports, &node_export_cost,
				&node_fee_domestic, &node_fee_export, &node_land_exports, &node_sea_exports, &node_imports, &node_covered
			}) {
				array->assign(node_count, fixed_point_t::_0);
			}
			route_flows.clear();
		}
		TypedSpan<country_index_t, fixed_point_t> supply_per_country = routed
			? TypedSpan<country_index_t, fixed_point_t> { node_supply }
			: reusable_country_map_0;
		TypedSpan<country_index_t, fixed_point_t> actual_bought_per_country = routed
			? TypedSpan<country_index_t, fixed_point_t> { node_bought }
			: reusable_country_map_1;
		for (GoodMarketSellOrder const& market_sell_order : market_sell_orders) {
			const std::optional<country_index_t> country_index_optional = market_sell_order.country_index_optional;
			if (routed) {
				supply_per_country[node_of(country_index_optional)] += market_sell_order.quantity;
			} else if (country_index_optional.has_value()) {
				supply_per_country[country_index_optional.value()] += market_sell_order.quantity;
			}
			supply_sum += market_sell_order.quantity;
		}
		memory::vector<fixed_point_t>& quantity_bought_per_order = reusable_vectors[0];
		memory::vector<fixed_point_t>& purchasing_power_per_order = reusable_vectors[1];
		quantity_bought_per_order.resize(buy_up_to_orders.size());
		purchasing_power_per_order.resize(buy_up_to_orders.size());

		// The most every buyer may buy today. Without the network it is what it asked for. With the network the buyers of a country can
		// buy at most what its sellers offer plus what the routes bring: the rest of its wishes is cut in proportion.
		effective_max_quantity.resize(buy_up_to_orders.size());
		fixed_point_t supply_for_clearing = supply_sum;
		if (routed) {
			const auto desire_of = [this](GoodBuyUpToOrder const& buy_up_to_order) -> fixed_point_t {
				if (buy_up_to_order.money_to_spend <= 0) {
					return fixed_point_t::_0;
				}
				return std::min(buy_up_to_order.max_quantity, buy_up_to_order.money_to_spend / max_next_price);
			};
			for (std::size_t i = 0; i < buy_up_to_orders.size(); i++) {
				node_desire[type_safe::get(node_of(buy_up_to_orders[i].country_index_optional))] += desire_of(buy_up_to_orders[i]);
			}
			supply_for_clearing = plan_routes();
			for (std::size_t i = 0; i < buy_up_to_orders.size(); i++) {
				GoodBuyUpToOrder const& buy_up_to_order = buy_up_to_orders[i];
				const std::size_t node = type_safe::get(node_of(buy_up_to_order.country_index_optional));
				const fixed_point_t desire_of_node = node_desire[node];
				const fixed_point_t cap = node_cap[node];
				if (desire_of_node > cap) {
					const fixed_point_t desire = desire_of(buy_up_to_order);
					effective_max_quantity[i] = desire > 0 ? fp::mul_div(desire, cap, desire_of_node) : buy_up_to_order.max_quantity;
				} else {
					effective_max_quantity[i] = buy_up_to_order.max_quantity;
				}
			}
		} else {
			for (std::size_t i = 0; i < buy_up_to_orders.size(); i++) {
				effective_max_quantity[i] = buy_up_to_orders[i].max_quantity;
			}
		}

		fixed_point_t money_left_to_spend_sum = 0; //sum of money_to_spend for all buyers that can't afford their max_quantity
		fixed_point_t max_quantity_to_buy_sum = 0;
		fixed_point_t purchasing_power_sum = 0;
		fixed_point_t affordable_wish_sum = 0; // what the buyers can pay for at the highest price, whether the routes can bring it or not
		for (size_t i = 0; i < buy_up_to_orders.size(); i++) {
			GoodBuyUpToOrder const& buy_up_to_order = buy_up_to_orders[i];
			const fixed_point_t max_quantity = effective_max_quantity[i];
			const fixed_point_t money_to_spend = buy_up_to_order.money_to_spend;

			if (game_rules_manager.get_use_optimal_pricing()) {
				const fixed_point_t affordable_price = buy_up_to_order.get_affordable_price();
				if (affordable_price > min_next_price) {
					//no point selling lower as it would not attract more buyers
					min_next_price = affordable_price;
				}
			}

			demand_sum += buy_up_to_order.max_quantity;

			if (money_to_spend <= 0) {
				purchasing_power_per_order[i] = 0;
				continue;
			}

			fixed_point_t purchasing_power = purchasing_power_per_order[i] = money_to_spend / max_next_price;
			affordable_wish_sum += std::min(purchasing_power, buy_up_to_order.max_quantity);
			if (purchasing_power >= max_quantity) {
				if (routed) {
					// The weight of the buyer is what it really wants (its sum is later reduced by the same number)
					purchasing_power_per_order[i] = max_quantity;
				}
				max_quantity_to_buy_sum += max_quantity;
				money_left_to_spend_sum += max_quantity * max_next_price;
				purchasing_power_sum += max_quantity;
			} else {
				max_quantity_to_buy_sum += purchasing_power;
				money_left_to_spend_sum += money_to_spend;
				purchasing_power_sum += purchasing_power;
			}
		}

		fixed_point_t remaining_supply = supply_for_clearing;
		// Nothing that is on offer can reach a buyer: nobody trades and the price stays
		const bool nothing_reachable = routed && supply_for_clearing <= fixed_point_t::_0;
		// The price rises when the buyers can pay for all that is on offer. The wishes that the routes cannot serve (and the offers that they cannot
		// carry) are not part of the market of the day, they are taken off both sides. Without the network it is the old rule.
		const fixed_point_t demand_for_price = routed ? affordable_wish_sum - cut_off_quantity : max_quantity_to_buy_sum;
		const bool is_selling_for_max_price = !nothing_reachable && demand_for_price >= supply_for_clearing;
		if (is_selling_for_max_price) {
			//sell for max_next_price
			if (game_rules_manager.get_use_optimal_pricing()) {
				new_price = max_next_price;
			} else {
				//TODO use Victoria 2's square root mechanic, see https://github.com/OpenVicProject/OpenVic/issues/288
				new_price = max_next_price;
			}

			bool someone_bought_max_quantity = false;
			if (routed) {
				// The supply is shared in proportion to the purchasing power, a buyer that would get more than it wants gets what it wants and the
				// rest is shared again (every round of the loop only looks at the buyers once, the buyers of a capped country all reach their
				// limit at the same time, so the search that starts over after each of them would be far too slow)
				std::vector<char> is_full(buy_up_to_orders.size(), 0);
				bool again = true;
				while (again && purchasing_power_sum > fixed_point_t::_0) {
					again = false;
					for (size_t i = 0; i < buy_up_to_orders.size(); i++) {
						if (is_full[i]) {
							continue;
						}
						if (purchasing_power_sum <= fixed_point_t::_0) {
							break;
						}
						// The most the buyer can take: what it wants or what its money pays for (the weight is the smaller of the two)
						const fixed_point_t limit = purchasing_power_per_order[i];
						const fixed_point_t share = fp::mul_div(remaining_supply, limit, purchasing_power_sum);
						if (share >= limit) {
							is_full[i] = 1;
							quantity_bought_per_order[i] = limit;
							remaining_supply -= limit;
							purchasing_power_sum -= limit;
							again = true;
						}
					}
				}
				for (size_t i = 0; i < buy_up_to_orders.size(); i++) {
					if (!is_full[i]) {
						quantity_bought_per_order[i] = purchasing_power_sum > fixed_point_t::_0
							? fp::mul_div(remaining_supply, purchasing_power_per_order[i], purchasing_power_sum)
							: fixed_point_t::_0;
					}
					actual_bought_per_country[node_of(buy_up_to_orders[i].country_index_optional)] += quantity_bought_per_order[i];
				}
			}
			if (!routed) do {
				someone_bought_max_quantity = false;
				for (size_t i = 0; i < buy_up_to_orders.size(); i++) {
					GoodBuyUpToOrder const& buy_up_to_order = buy_up_to_orders[i];
					const fixed_point_t max_quantity = effective_max_quantity[i];
					fixed_point_t& distributed_supply = quantity_bought_per_order[i];
					if (distributed_supply == max_quantity) {
						continue;
					}

					const std::optional<country_index_t> country_index_optional = buy_up_to_order.country_index_optional;
					if (routed || country_index_optional.has_value()) {
						//subtract as it might be updated below
						actual_bought_per_country[node_of(country_index_optional)] -= distributed_supply;
					}

					distributed_supply = purchasing_power_sum > fixed_point_t::_0
						? fp::mul_div(
							remaining_supply,
							purchasing_power_per_order[i],
							purchasing_power_sum
						)
						: fixed_point_t::_0;

					if (distributed_supply >= max_quantity) {
						someone_bought_max_quantity = true;
						distributed_supply = max_quantity;
						remaining_supply -= max_quantity;
						purchasing_power_sum -= purchasing_power_per_order[i];
					}

					if (routed || country_index_optional.has_value()) {
						actual_bought_per_country[node_of(country_index_optional)] += distributed_supply;
					}

					if (someone_bought_max_quantity) {
						break;
					}
				}
			} while (someone_bought_max_quantity);

			execute_buy_orders(
				new_price,
				actual_bought_per_country,
				supply_per_country,
				quantity_bought_per_order
			);
		} else {
			//sell below max_next_price
			if (game_rules_manager.get_use_optimal_pricing()) {
				new_price = price;

				//drop price while remaining_supply > 0 && new_price > min_next_price
				while (remaining_supply > 0) {
					const fixed_point_t possible_price = money_left_to_spend_sum / remaining_supply;

					if (possible_price >= new_price) {
						//use previous new_price
						break;
					}

					if (possible_price < min_next_price) {
						new_price = min_next_price;
						break;
					}

					new_price = possible_price;

					for (size_t i = 0; i < buy_up_to_orders.size(); i++) {
						GoodBuyUpToOrder const& buy_up_to_order = buy_up_to_orders[i];
						if (quantity_bought_per_order[i] == effective_max_quantity[i]) {
							continue;
						}

						if (buy_up_to_order.money_to_spend >= new_price * effective_max_quantity[i]) {
							quantity_bought_per_order[i] = effective_max_quantity[i];
							remaining_supply -= effective_max_quantity[i];
							money_left_to_spend_sum -= buy_up_to_order.money_to_spend;
						}
					}
				}
			} else {
				// Only the demand that can be paid moves the price up (the branch above: the buyers can afford the whole supply at the
				// highest price of the day). When they cannot, the price falls by the step of a day, whatever they want. Before, a good
				// that was wanted more than offered but could not be paid for kept the same price for ever (stuck prices), and
				// counting the unpaid demand instead made the prices of everything that is scarce run to the upper bound.
				//TODO use Victoria 2's square root mechanic for the size of the step, see https://github.com/OpenVicProject/OpenVic/issues/288
				new_price = nothing_reachable ? price : min_next_price;
			}

			//figure out how much every buyer bought
			for (size_t i = 0; i < buy_up_to_orders.size(); i++) {
				GoodBuyUpToOrder const& buy_up_to_order = buy_up_to_orders[i];

				const fixed_point_t quantity_bought
					= quantity_bought_per_order[i]
					= std::min(
					effective_max_quantity[i],
					buy_up_to_order.money_to_spend / new_price
				);

				const std::optional<country_index_t> country_index_optional = buy_up_to_order.country_index_optional;
				if (!routed && country_index_optional.has_value()) {
					actual_bought_per_country[country_index_optional.value()] += quantity_bought_per_order[i];
				}
			}

			if (routed) {
				// At the lower price the buyers can afford a little more than there is: they get no more than the offers that can reach them
				fixed_point_t total_bought = fixed_point_t::_0;
				for (size_t i = 0; i < buy_up_to_orders.size(); i++) {
					total_bought += quantity_bought_per_order[i];
				}
				const bool scale_down = total_bought > supply_for_clearing && total_bought > fixed_point_t::_0;
				for (size_t i = 0; i < buy_up_to_orders.size(); i++) {
					if (scale_down) {
						quantity_bought_per_order[i] = fp::mul_div(quantity_bought_per_order[i], supply_for_clearing, total_bought);
					}
					actual_bought_per_country[node_of(buy_up_to_orders[i].country_index_optional)] += quantity_bought_per_order[i];
				}
			}

			execute_buy_orders(
				new_price,
				actual_bought_per_country,
				supply_per_country,
				quantity_bought_per_order
			);
		}

		for (auto& reusable_vector : reusable_vectors) {
			reusable_vector.clear();
		}

		//figure out how much of each order was sold
		if (routed) {
			attribute_routed_sales(new_price, reusable_vectors[0]);
		} else if (quantity_traded_yesterday == supply_sum) {
			//everything was sold
			for (GoodMarketSellOrder const& market_sell_order : market_sell_orders) {
				const fixed_point_t quantity_sold = market_sell_order.quantity;
				fixed_point_t money_gained;
				if (quantity_sold == 0) {
					money_gained = 0;
				} else {
					money_gained = std::max(
						quantity_sold * new_price,
						fixed_point_t::epsilon //round up
					);
				}
				MoneyLedger::record(MoneyLedger::label_t::market_sellers_get, money_gained);
				market_sell_order.call_after_trade(
					{
						good_definition.index,
						quantity_sold,
						money_gained
					},
					reusable_vectors[0]
				);
			}
		} else {
			//quantity is evenly divided after taking domestic buyers into account
			fixed_point_t total_quantity_traded_domestically = 0;
			for (country_index_t country_index(0); country_index < actual_bought_per_country.size(); ++country_index) {
				const fixed_point_t actual_bought = actual_bought_per_country[country_index];
				const fixed_point_t supply = supply_per_country[country_index];
				const fixed_point_t traded_domestically = std::min(supply, actual_bought);
				total_quantity_traded_domestically += traded_domestically;
			}

			const fixed_point_t total_quantity_traded_as_export = quantity_traded_yesterday - total_quantity_traded_domestically;
			const fixed_point_t total_quantity_offered_as_export = supply_sum - total_quantity_traded_domestically;
			for (GoodMarketSellOrder const& market_sell_order : market_sell_orders) {
				const fixed_point_t quantity_offered = market_sell_order.quantity;

				fixed_point_t quantity_sold_domestically;
				fixed_point_t quantity_offered_as_export;
				const std::optional<country_index_t> country_index_optional = market_sell_order.country_index_optional;
				if (!country_index_optional.has_value()) {
					quantity_sold_domestically = 0;
					quantity_offered_as_export = quantity_offered;
				} else {
					const country_index_t country_index = country_index_optional.value();
					const fixed_point_t total_bought_domestically = actual_bought_per_country[country_index];
					const fixed_point_t total_domestic_supply = supply_per_country[country_index];
					quantity_sold_domestically = total_bought_domestically >= total_domestic_supply
						? quantity_offered
						: fp::mul_div(
							quantity_offered,
							total_bought_domestically,
							total_domestic_supply //> 0 as we're selling
						);
					quantity_offered_as_export = quantity_offered - quantity_sold_domestically;
				}

				const fixed_point_t fair_share_of_exports = fp::mul_div(
					quantity_offered_as_export,
					total_quantity_traded_as_export,
					total_quantity_offered_as_export
				);

				const fixed_point_t quantity_sold = quantity_sold_domestically + fair_share_of_exports;
				fixed_point_t money_gained;
				if (quantity_sold == 0) {
					money_gained = 0;
				} else {
					money_gained = std::max(
						quantity_sold * new_price,
						fixed_point_t::epsilon //round up
					);
				}
				MoneyLedger::record(MoneyLedger::label_t::market_sellers_get, money_gained);
				market_sell_order.call_after_trade(
					{
						good_definition.index,
						quantity_sold,
						money_gained
					},
					reusable_vectors[0]
				);
			}
		}

		std::fill(reusable_country_map_0.begin(), reusable_country_map_0.end(), 0);
		std::fill(reusable_country_map_1.begin(), reusable_country_map_1.end(), 0);
		market_sell_orders.clear();
		std::fill(supply_per_country.begin(), supply_per_country.end(), 0);
		std::fill(actual_bought_per_country.begin(), actual_bought_per_country.end(), 0);
	}

	price_change_yesterday = new_price - price;
	total_demand_yesterday = demand_sum;
	total_supply_yesterday = supply_sum;
	buy_up_to_orders.clear();
	if (new_price != price) {
		price = new_price;
		update_next_price_limits();
	}
}

void GoodMarket::execute_buy_orders(
	const fixed_point_t new_price,
	TypedSpan<country_index_t, const fixed_point_t> actual_bought_per_country,
	TypedSpan<country_index_t, const fixed_point_t> supply_per_country,
	std::span<const fixed_point_t> quantity_bought_per_order
) {
	quantity_traded_yesterday = 0;
	for (size_t i = 0; i < buy_up_to_orders.size(); i++) {
		GoodBuyUpToOrder const& buy_up_to_order = buy_up_to_orders[i];
		const fixed_point_t quantity_bought = quantity_bought_per_order[i];

		if (quantity_bought == 0) {
			buy_up_to_order.call_after_trade(BuyResult::no_purchase_result(good_definition.index));
		} else {
			quantity_traded_yesterday += quantity_bought;
			const fixed_point_t money_spent_total = std::max(
				quantity_bought * new_price,
				fixed_point_t::epsilon //we know from purchasing power that you can afford it.
			);
			MoneyLedger::record(MoneyLedger::label_t::market_buyers_pay, -money_spent_total);

			fixed_point_t money_spent_on_imports;
			const std::optional<country_index_t> country_index_optional = buy_up_to_order.country_index_optional;
			if (!country_index_optional.has_value()) {
				//could be trade between native Americans and tribal Africa, so it's all imported
				money_spent_on_imports = money_spent_total;
			} else {
				const country_index_t country_index = country_index_optional.value();
				//must be > 0, since quantity_bought > 0
				const fixed_point_t actual_bought_in_my_country = actual_bought_per_country[country_index];
				const fixed_point_t supply_in_my_country = supply_per_country[country_index];

				if (supply_in_my_country >= actual_bought_in_my_country) {
					//no imports
					money_spent_on_imports = 0;
				} else {
					const fixed_point_t money_spent_domestically = fp::mul_div(
						money_spent_total,
						supply_in_my_country,
						actual_bought_in_my_country
					);

					money_spent_on_imports = money_spent_total - money_spent_domestically;
				}
			}
			buy_up_to_order.call_after_trade({
				good_definition.index,
				quantity_bought,
				money_spent_total,
				money_spent_on_imports
			});
		}
	}
}

void GoodMarket::record_price_history() {
	price_history.push_back(price);
}

fixed_point_t GoodMarket::plan_routes() {
	const std::size_t node_count = node_supply.size();
	struct candidate_t {
		fixed_point_t cost;
		fixed_point_t room;
		std::uint32_t from;
		std::uint32_t to;
		std::uint8_t mode;
	};

	// What a node cannot use itself is offered abroad, what its sellers cannot fill is wished from abroad
	std::vector<fixed_point_t> export_left(node_count, fixed_point_t::_0), import_left(node_count, fixed_point_t::_0);
	fixed_point_t sold_at_home = fixed_point_t::_0, total_supply = fixed_point_t::_0, total_desire = fixed_point_t::_0;
	for (std::size_t node = 0; node < node_count; ++node) {
		const fixed_point_t supply = node_supply[node];
		const fixed_point_t desire = node_desire[node];
		total_supply += supply;
		total_desire += desire;
		sold_at_home += std::min(supply, desire);
		if (supply > desire) {
			export_left[node] = supply - desire;
		} else if (desire > supply) {
			import_left[node] = desire - supply;
		}
	}

	std::vector<candidate_t> candidates;
	for (std::size_t from = 0; from < node_count; ++from) {
		if (export_left[from] <= fixed_point_t::_0) {
			continue;
		}
		for (std::size_t to = 0; to < node_count; ++to) {
			if (to == from || import_left[to] <= fixed_point_t::_0) {
				continue;
			}
			TradeNetwork::route_t const& route = trade_network->get_route(country_index_t(from), country_index_t(to));
			if (route.mode == TradeNetwork::mode_t::NONE) {
				continue;
			}
			// The capacity is in pounds of cargo value, the quantity is what that buys at the price of today
			const fixed_point_t room = route.capacity_value < fixed_point_t::_0 ? fixed_point_t::usable_max : route.capacity_value * price_inverse;
			if (room <= fixed_point_t::_0) {
				continue;
			}
			candidates.push_back({
				route.cost, room, static_cast<std::uint32_t>(from), static_cast<std::uint32_t>(to), static_cast<std::uint8_t>(route.mode)
			});
		}
	}

	// The cheapest routes first (ties by the countries, so that the order of the orders and of the threads does not matter)
	std::sort(candidates.begin(), candidates.end(), [](candidate_t const& lhs, candidate_t const& rhs) -> bool {
		if (lhs.cost != rhs.cost) {
			return lhs.cost < rhs.cost;
		}
		if (lhs.from != rhs.from) {
			return lhs.from < rhs.from;
		}
		return lhs.to < rhs.to;
	});

	fixed_point_t planned = fixed_point_t::_0;
	for (candidate_t const& candidate : candidates) {
		const fixed_point_t quantity = std::min({ export_left[candidate.from], import_left[candidate.to], candidate.room });
		if (quantity <= fixed_point_t::_0) {
			continue;
		}
		export_left[candidate.from] -= quantity;
		import_left[candidate.to] -= quantity;
		node_inflow[candidate.to] += quantity;
		planned += quantity;
		route_flows.push_back({ candidate.from, candidate.to, quantity, candidate.cost, candidate.mode });
	}

	for (std::size_t node = 0; node < node_count; ++node) {
		node_cap[node] = node_supply[node] + node_inflow[node];
	}
	// What is left on offer in one country and wanted in another (min of the two sums, as every pair of them had no route or no room left)
	// is cut off from the market: it does not count for the price. What is left on offer when nobody wants it is a glut, it does count.
	cut_off_quantity = std::max(std::min(total_supply, total_desire) - sold_at_home - planned, fixed_point_t::_0);
	if (cut_off_quantity > fixed_point_t::_0) {
		trade_network->add_stat(trade_network->get_outside_node(), 4, cut_off_quantity * price);
	}
	return total_supply - cut_off_quantity;
}

void GoodMarket::attribute_routed_sales(const fixed_point_t new_price, memory::vector<fixed_point_t>& reusable_vector) {
	const std::size_t node_count = node_supply.size();
	const std::size_t outside = type_safe::get(trade_network->get_outside_node());

	// What every node sold at home and what it bought from abroad
	for (std::size_t node = 0; node < node_count; ++node) {
		node_dom[node] = node == outside ? fixed_point_t::_0 : std::min(node_supply[node], node_bought[node]);
		node_imports[node] = node_bought[node] - node_dom[node];
	}

	// The planned flows, cut down to what the importers really bought
	for (route_flow_t const& flow : route_flows) {
		const fixed_point_t planned_inflow = node_inflow[flow.to];
		const fixed_point_t bought = node_imports[flow.to];
		// (rounded up by the smallest amount: the sellers must get at least what the buyers pay)
		fixed_point_t quantity = flow.quantity;
		if (bought < planned_inflow) {
			quantity = fp::mul_div(flow.quantity, bought, planned_inflow);
			if (quantity > fixed_point_t::_0) {
				quantity = std::min(flow.quantity, quantity + fixed_point_t::epsilon);
			}
		}
		if (quantity <= fixed_point_t::_0) {
			continue;
		}
		trade_network->add_pair_flow(country_index_t(flow.from), country_index_t(flow.to), quantity * new_price);
		node_exports[flow.from] += quantity;
		node_export_cost[flow.from] += quantity * flow.cost;
		node_covered[flow.to] += quantity;
		if (flow.mode == static_cast<std::uint8_t>(TradeNetwork::mode_t::LAND)) {
			node_land_exports[flow.from] += quantity;
		} else {
			node_sea_exports[flow.from] += quantity;
		}
	}

	// Imports that the plan did not foresee (buyers that bought more when the price fell) come from what the exporters have left, at the spot freight
	fixed_point_t unplanned = fixed_point_t::_0, room = fixed_point_t::_0;
	for (std::size_t node = 0; node < node_count; ++node) {
		if (node_imports[node] > node_covered[node]) {
			unplanned += node_imports[node] - node_covered[node];
		}
		const fixed_point_t left = node_supply[node] - node_dom[node] - node_exports[node];
		if (left > fixed_point_t::_0) {
			room += left;
		}
	}
	if (unplanned > fixed_point_t::_0 && room > fixed_point_t::_0) {
		const fixed_point_t take = std::min(unplanned, room);
		const fixed_point_t spot_cost = trade_network->get_spot_cost();
		for (std::size_t node = 0; node < node_count; ++node) {
			const fixed_point_t left = node_supply[node] - node_dom[node] - node_exports[node];
			if (left <= fixed_point_t::_0) {
				continue;
			}
			fixed_point_t extra = fp::mul_div(take, left, room);
			if (extra > fixed_point_t::_0) {
				extra = std::min(left, extra + fixed_point_t::epsilon);
			}
			node_exports[node] += extra;
			node_export_cost[node] += extra * spot_cost;
			node_sea_exports[node] += extra;
		}
	}

	// From the sum of the freight to its average share of the price
	for (std::size_t node = 0; node < node_count; ++node) {
		node_export_cost[node] = node_exports[node] > fixed_point_t::_0 ? node_export_cost[node] / node_exports[node] : fixed_point_t::_0;
	}

	for (GoodMarketSellOrder const& market_sell_order : market_sell_orders) {
		const std::size_t node = type_safe::get(
			market_sell_order.country_index_optional.has_value() ? market_sell_order.country_index_optional.value() : trade_network->get_outside_node()
		);
		const bool is_country = node != outside;
		const fixed_point_t quantity_offered = market_sell_order.quantity;
		const fixed_point_t supply = node_supply[node]; //> 0 as we're selling

		// A share of the order: rounded up (by the smallest amount), so that what the sellers get is never less than what the buyers pay
		const auto share_of_order = [quantity_offered, supply](const fixed_point_t part) -> fixed_point_t {
			if (part >= supply) {
				return quantity_offered;
			}
			const fixed_point_t share = fp::mul_div(quantity_offered, part, supply);
			return share > fixed_point_t::_0 ? std::min(quantity_offered, share + fixed_point_t::epsilon) : share;
		};
		const fixed_point_t sold_at_home = share_of_order(node_dom[node]);
		fixed_point_t sold_abroad = share_of_order(node_exports[node]);
		if (sold_at_home + sold_abroad > quantity_offered) {
			sold_abroad = quantity_offered - sold_at_home;
		}

		const fixed_point_t quantity_sold = sold_at_home + sold_abroad;
		fixed_point_t money_gained;
		if (quantity_sold == 0) {
			money_gained = 0;
		} else {
			const fixed_point_t gross = std::max(
				quantity_sold * new_price,
				fixed_point_t::epsilon //round up
			);
			// The freight: at home the share of the seller's province, abroad that plus the share of the route
			const fixed_point_t rate_home = is_country ? market_sell_order.freight_rate : fixed_point_t::_0;
			const fixed_point_t rate_route = is_country ? node_export_cost[node] : fixed_point_t::_0;
			fixed_point_t fee_home = sold_at_home * new_price * rate_home;
			fixed_point_t fee_abroad = sold_abroad * new_price * (rate_home + rate_route - rate_home * rate_route);
			if (fee_home + fee_abroad >= gross) {
				// Rounding: the seller keeps at least the smallest amount
				fee_home = fixed_point_t::_0;
				fee_abroad = fixed_point_t::_0;
			}
			money_gained = gross - fee_home - fee_abroad;
			node_fee_domestic[node] += fee_home;
			node_fee_export[node] += fee_abroad;
		}
		MoneyLedger::record(MoneyLedger::label_t::market_sellers_get, money_gained);
		market_sell_order.call_after_trade(
			{
				good_definition.index,
				quantity_sold,
				money_gained
			},
			reusable_vector
		);
	}

	// The freight goes to the treasuries, and the statistics of the day
	for (std::size_t node = 0; node < node_count; ++node) {
		const country_index_t country_index(node);
		if (node != outside) {
			trade_network->add_fee(country_index, node_fee_domestic[node] + node_fee_export[node]);
			trade_network->add_stat(country_index, 2, node_fee_domestic[node]);
			trade_network->add_stat(country_index, 3, node_fee_export[node]);
		}
		trade_network->add_stat(country_index, 0, node_exports[node] * new_price);
		trade_network->add_stat(country_index, 1, node_imports[node] * new_price);
		trade_network->add_stat(country_index, 5, node_land_exports[node] * new_price);
		trade_network->add_stat(country_index, 6, node_sea_exports[node] * new_price);
	}
}

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "openvic-simulation/core/object/FixedPoint.hpp"
#include "openvic-simulation/types/TypedIndices.hpp"

namespace OpenVic {
	struct CountryInstance;
	struct InstanceManager;
	struct OpenVicTradeDefines;

	/* The roads of world trade (docs/TRADE.md). Every country is a node, the last node is "outside" (provinces that nobody owns).
	 * Once a month (and when the game starts) the network looks at the borders and the ports of the countries and builds
	 *  - land links: two countries are linked by every pair of their neighbouring land provinces; railroads on both sides make
	 *    a crossing cheaper and its capacity bigger,
	 *  - sea links: every country with a port is linked with every other one; the cost grows with the sea distance between the
	 *    ports, naval bases make it cheaper and the capacity bigger,
	 *  - routes: the cheapest way between two countries (at most MAX_ROUTE_HOPS links, other countries may be passed),
	 *    with its cost (the share of the price that the seller of the goods loses) and the capacity of its narrowest link.
	 * It also sets for every province the freight of the goods that leave it for the market of its own country (it depends on
	 * the railroad of the province). The market (GoodMarket) only reads all this while it runs.
	 * Everything is a function of the state of the game at the start of a month, so replays give the same numbers. */
	struct TradeNetwork {
		enum struct mode_t : std::uint8_t { NONE, LAND, SEA, MIXED };

		struct route_t {
			// Share of the price the seller loses on the way, negative if the two countries cannot trade
			fixed_point_t cost = -1;
			// Pounds of cargo value per day for every good, negative: no limit
			fixed_point_t capacity_value = 0;
			std::uint8_t hops = 0;
			mode_t mode = mode_t::NONE;
		};

		// Totals of a day by country (the market adds to them from all threads), used for the statistics and the debugger
		struct country_stats_t {
			fixed_point_t export_value, import_value; // cargo value (quantity * price) that left / came to the country
			fixed_point_t internal_fees, export_fees; // paid by the sellers of the country, they go to its treasury
			fixed_point_t unmet_import_value; // (kept at the outside node) on offer in one country and wanted in another, but the routes had no room
			fixed_point_t land_export_value, sea_export_value;
		};

	private:
		OpenVicTradeDefines const& defines;
		const std::size_t country_count;
		std::span<CountryInstance> countries;
		bool ready = false;

		std::vector<route_t> routes; // (country_count + 1) squared, [from * node_count + to]
		std::vector<fixed_point_t> sea_capacity; // by country: the sum of its ports
		std::vector<std::int32_t> land_crossings; // by country: neighbouring pairs of provinces with other countries
		std::vector<fixed_point_t> best_rail; // by country: the best railroad level at the border (0..1)
		std::vector<fixed_point_t> raw_rail; // by province: its own railroad level (0..1)

		// The sea zones (static), the distance between every two of them in map pixels (negative: not connected)
		bool zones_built = false;
		std::vector<std::int32_t> zone_of_province;
		std::size_t zone_count = 0;
		std::vector<std::int32_t> zone_distance;

		// Written by all threads while the market runs, read by the countries later
		std::vector<std::atomic<std::int64_t>> today_stats; // 8 numbers by node
		std::vector<country_stats_t> yesterday_stats;
		// The cargo value that went from one country to another (index from * nodes + to), for the statistics
		std::vector<std::atomic<std::int64_t>> today_pair_flow;
		std::vector<fixed_point_t> yesterday_pair_flow;

		void build_zones(InstanceManager& instance_manager);

	public:
		static constexpr std::size_t STAT_COUNT = 7;

		TradeNetwork(OpenVicTradeDefines const& new_defines, std::size_t new_country_count);

		// The countries that get the fees of the market
		void set_countries(std::span<CountryInstance> new_countries) {
			countries = new_countries;
		}
		bool is_enabled() const;
		bool is_ready() const {
			return ready;
		}
		// Builds the sea zones of the trade (they depend only on the definitions of the map, not on the state of the game), so that the
		// first day does not have to; called when the game session starts
		void prepare(InstanceManager& instance_manager) {
			if (!zones_built) {
				build_zones(instance_manager);
			}
		}
		// Builds the links and routes from the current state of the map; called once a month and before the first day
		void update(InstanceManager& instance_manager);

		std::size_t get_node_count() const {
			return country_count + 1;
		}
		// The node of the provinces that nobody owns
		country_index_t get_outside_node() const {
			return country_index_t(country_count);
		}
		route_t const& get_route(const country_index_t from, const country_index_t to) const {
			return routes[type_safe::get(from) * (country_count + 1) + type_safe::get(to)];
		}
		// The freight of the goods that the plan of the day did not foresee
		fixed_point_t get_spot_cost() const;
		fixed_point_t get_sea_capacity(const country_index_t country) const {
			return sea_capacity[type_safe::get(country)];
		}
		std::int32_t get_land_crossings(const country_index_t country) const {
			return land_crossings[type_safe::get(country)];
		}

		// Thread safe, the order of the additions does not matter
		void add_stat(const country_index_t country, const std::size_t stat, const fixed_point_t amount);
		// The fee of the sellers goes to the treasury of the country that sold; thread safe
		void add_fee(const country_index_t country, const fixed_point_t amount);
		void add_pair_flow(const country_index_t from, const country_index_t to, const fixed_point_t value);
		fixed_point_t get_pair_flow_yesterday(const country_index_t from, const country_index_t to) const {
			return yesterday_pair_flow[type_safe::get(from) * (country_count + 1) + type_safe::get(to)];
		}
		// Moves the totals of the day to the statistics of yesterday and starts a new day (not thread safe)
		void begin_day();
		country_stats_t const& get_stats_yesterday(const country_index_t country) const {
			return yesterday_stats[type_safe::get(country)];
		}
	};
}

#pragma once

#include <cstddef>
#include <vector>

namespace OpenVic {
	struct InstanceManager;

	/* The report for the calibration of the economy (docs/BALANCE_TARGETS.md), printed with the prefix "BALANCE" (summary of the
	 * targets T1-T10) and "GOODS" (one line per good: price against the base price, demand, supply, share traded).
	 * Call update() every day (it follows how long a price has stayed the same) and print() when a line is wanted. */
	struct BalanceReport {
	private:
		std::vector<int> unchanged_days; // per good: days in a row with the same price
		std::vector<long long> last_price; // per good: raw price of the last day
		double start_population = 0.0;
		long long start_day = -1;

	public:
		void update(InstanceManager& instance_manager);
		void print(InstanceManager& instance_manager, bool with_goods);
	};
}

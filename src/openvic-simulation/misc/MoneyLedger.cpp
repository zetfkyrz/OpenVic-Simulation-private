#include "MoneyLedger.hpp"

#include <cmath>

#include <fmt/format.h>

#include "openvic-simulation/InstanceManager.hpp"
#include "openvic-simulation/country/CountryInstance.hpp"
#include "openvic-simulation/economy/production/FactoryInstance.hpp"
#include "openvic-simulation/map/MapInstance.hpp"
#include "openvic-simulation/map/ProvinceInstance.hpp"
#include "openvic-simulation/map/State.hpp"
#include "openvic-simulation/population/Pop.hpp"

using namespace OpenVic;

namespace {
	constexpr char const* label_names[] = {
#define OV_MONEY_NAME(name, category) #name,
		OV_MONEY_LABELS(OV_MONEY_NAME)
#undef OV_MONEY_NAME
	};
	constexpr char const* label_categories[] = {
#define OV_MONEY_CATEGORY(name, category) category,
		OV_MONEY_LABELS(OV_MONEY_CATEGORY)
#undef OV_MONEY_CATEGORY
	};

	double to_double(const std::int64_t raw) {
		return static_cast<double>(raw) / static_cast<double>(1 << fixed_point_t::PRECISION);
	}
}

MoneyLedger::Totals MoneyLedger::measure(InstanceManager& instance_manager) {
	Totals totals;
	for (ProvinceInstance& province : instance_manager.get_map_instance().get_province_instances()) {
		for (Pop& pop : province.get_mutable_pops()) {
			totals.pops += pop.get_cash().get_copy_of_value();
		}
	}
	for (CountryInstance const& country : instance_manager.get_country_instance_manager().get_country_instances()) {
		totals.countries += country.get_cash_stockpile().load();
	}
	for (StateSet const& state_set : instance_manager.get_map_instance().get_state_manager().get_state_sets()) {
		for (State const& state : state_set.get_states()) {
			totals.capitalist_funds += state.get_capitalist_fund();
			for (FactoryInstance const& factory : state.get_factories()) {
				totals.factory_budgets += factory.get_budget();
				totals.expansion_funds += factory.get_expansion_fund();
			}
		}
	}
	return totals;
}

void MoneyLedger::enable(InstanceManager& instance_manager) {
	active = this;
	last_totals = measure(instance_manager);
	start_total = period_start_total = last_totals.total().get_raw_value();
	started = true;
}

void MoneyLedger::disable() {
	if (active == this) {
		active = nullptr;
	}
}

void MoneyLedger::begin_day(InstanceManager& instance_manager) {
	if (!is_enabled()) {
		return;
	}
	last_totals = measure(instance_manager);
}

void MoneyLedger::end_phase(InstanceManager& instance_manager, std::string const& phase) {
	if (!is_enabled()) {
		return;
	}
	const Totals now = measure(instance_manager);
	const std::int64_t delta = now.total().get_raw_value() - last_totals.total().get_raw_value();
	last_totals = now;
	for (auto& [name, total] : phase_totals) {
		if (name == phase) {
			total += delta;
			return;
		}
	}
	phase_totals.emplace_back(phase, delta);
}

void MoneyLedger::end_day(InstanceManager& instance_manager) {
	if (!is_enabled()) {
		return;
	}
	days++;
	// The flows of the day go to the totals
	for (std::size_t i = 0; i < LABEL_COUNT; ++i) {
		flow_totals[i] += flows[i].exchange(0, std::memory_order_relaxed);
	}
	(void)instance_manager;
}

std::string MoneyLedger::report(InstanceManager& instance_manager) {
	const Totals now = measure(instance_manager);
	const double total = to_double(now.total().get_raw_value());
	const double start = to_double(start_total);
	std::string text = fmt::format(
		"MONEY {} days={} total={:.0f} (pops {:.0f}, countries {:.0f}, factories {:.0f}, funds {:.0f}) change since the start {:+.2f}%",
		instance_manager.get_today(), days, total, to_double(now.pops.get_raw_value()), to_double(now.countries.get_raw_value()),
		to_double(now.factory_budgets.get_raw_value()),
		to_double((now.expansion_funds + now.capitalist_funds).get_raw_value()), start > 0 ? (total / start - 1.0) * 100.0 : 0.0
	);
	text += "\nMONEY phases (sum of the changes of the total money per phase, per day):";
	for (auto const& [name, delta] : phase_totals) {
		text += fmt::format(" {}={:+.1f}", name, days > 0 ? to_double(delta) / static_cast<double>(days) : 0.0);
	}
	// Labels that only describe one side of a payment that is also counted at the holder (market, gross incomes, ...)
	const auto is_holder_label = [](const std::size_t i) -> bool {
		const std::string_view name = label_names[i];
		return !(name.starts_with("market_") || name.starts_with("gross_income_") || name == "pop_tax_paid" || name == "rgo_revenue"
			|| name == "gold_worker_pay" || name.starts_with("sell_callbacks_") || name.starts_with("buy_callbacks_"));
	};
	std::int64_t holder_sum = 0;
	for (std::size_t i = 0; i < LABEL_COUNT; ++i) {
		const std::int64_t raw = flow_totals[i] + flows[i].load(std::memory_order_relaxed);
		if (is_holder_label(i)) {
			holder_sum += raw;
		}
	}
	const std::int64_t measured = now.total().get_raw_value() - start_total;
	text += fmt::format(
		"\nMONEY balance per day: change of the total {:+.1f}, sum of the changes of the holders {:+.1f}, not explained by the hooks {:+.1f}",
		days > 0 ? to_double(measured) / static_cast<double>(days) : 0.0,
		days > 0 ? to_double(holder_sum) / static_cast<double>(days) : 0.0,
		days > 0 ? to_double(measured - holder_sum) / static_cast<double>(days) : 0.0
	);
	for (std::size_t i = 0; i < LABEL_COUNT; ++i) {
		std::int64_t raw = flow_totals[i] + flows[i].load(std::memory_order_relaxed);
		if (raw == 0) {
			continue;
		}
		text += fmt::format("\nMONEY   {:<28} [{}] {:.1f} per day", label_names[i], label_categories[i],
			days > 0 ? to_double(raw) / static_cast<double>(days) : 0.0);
	}
	return text;
}

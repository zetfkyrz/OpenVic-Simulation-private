#include "FactoryInstance.hpp"

#include <algorithm>

#include "openvic-simulation/misc/MoneyLedger.hpp"
#include "openvic-simulation/country/CountryInstance.hpp"
#include "openvic-simulation/economy/BuildingType.hpp"
#include "openvic-simulation/economy/GoodDefinition.hpp"
#include "openvic-simulation/economy/GoodInstance.hpp"
#include "openvic-simulation/economy/production/ProductionType.hpp"
#include "openvic-simulation/economy/trading/BuyResult.hpp"
#include "openvic-simulation/economy/trading/BuyUpToOrder.hpp"
#include "openvic-simulation/economy/trading/MarketInstance.hpp"
#include "openvic-simulation/economy/trading/MarketSellOrder.hpp"
#include "openvic-simulation/economy/trading/SellResult.hpp"
#include "openvic-simulation/map/State.hpp"
#include "openvic-simulation/population/Pop.hpp"
#include "openvic-simulation/utility/Logger.hpp"

using namespace OpenVic;

namespace {
	// The parameters of the factories (budget, stockpile, wages, expansion, closing) are in the group ov_economy of the
	// defines, see FactoryInstance::defines
	OpenVicEconomyDefines const& get_defines() {
		return *FactoryInstance::defines;
	}
}

FactoryInstance::Input::Input(GoodDefinition const& new_good, const fixed_point_t new_quantity_per_level)
	: good { new_good }, quantity_per_level { new_quantity_per_level } {}

FactoryInstance::FactoryInstance(
	BuildingType const& new_building_type, ProductionType const& new_production_type, const building_level_t new_level
) : level { new_level },
	efficiency { fixed_point_t::_1 },
	throttle { fixed_point_t::_1 },
	building_type { new_building_type },
	production_type { new_production_type } {
	inputs.reserve(production_type.input_goods.size());
	for (auto const& [good, quantity] : production_type.input_goods) {
		inputs.emplace_back(*good, quantity);
	}
}

fixed_point_t FactoryInstance::calculate_construction_cost(
	BuildingType const& building_type, MarketInstance const& market_instance
) {
	fixed_point_t cost = building_type.cost;
	for (auto const& [good, quantity] : building_type.goods_cost) {
		cost += quantity * market_instance.get_good_instance(good->index).get_price();
	}
	return cost;
}

Timespan FactoryInstance::calculate_construction_time(BuildingType const& building_type) {
	const Timespan::value_t days = building_type.build_time.to_int();
	return Timespan::from_days(days > 0 ? days : get_defines().default_construction_days);
}

bool FactoryInstance::can_be_expanded() const {
	return !is_under_construction() && level < building_type.max_level;
}

fixed_point_t FactoryInstance::get_construction_progress(const Date today) const {
	if (!construction_start.has_value() || !construction_end.has_value()) {
		return 0;
	}

	const int32_t total = static_cast<int32_t>((*construction_end - *construction_start).to_int());
	if (total <= 0) {
		return 1;
	}

	const int32_t elapsed = std::clamp<int32_t>(static_cast<int32_t>((today - *construction_start).to_int()), 0, total);
	return fp::from_fraction(elapsed, total);
}

void FactoryInstance::start_construction(const Date today, const bool uses_stockpile) {
	if (is_under_construction()) {
		return;
	}

	const Timespan time = calculate_construction_time(building_type);
	construction_start = today;
	construction_end = today + time;
	construction_uses_stockpile = uses_stockpile;
	construction_pace = fixed_point_t::_0;
	construction_total_days = std::max<int32_t>(1, static_cast<int32_t>(time.to_int()));
}

fixed_point_t FactoryInstance::get_daily_construction_goods(GoodDefinition const& good) const {
	const auto it = building_type.goods_cost.find(&good);
	if (it == building_type.goods_cost.end()) {
		return fixed_point_t::_0;
	}
	return it->second / construction_total_days;
}

fixed_point_t FactoryInstance::calculate_value(MarketInstance const& market_instance) const {
	const fixed_point_t replacement = calculate_construction_cost(building_type, market_instance) *
		static_cast<int32_t>(type_safe::get(level)) / get_defines().factory_replacement_value_divisor;
	const fixed_point_t from_profit = average_profit > fixed_point_t::_0
		? average_profit * (365 * get_defines().factory_value_payback_years)
		: fixed_point_t::_0;
	return std::max(from_profit, replacement);
}

bool FactoryInstance::trade_share(
	State& state, CountryInstance& country, MarketInstance const& market_instance, fixed_point_t share, const bool state_buys
) {
	if (level <= building_level_t(0) || share <= fixed_point_t::_0) {
		return false;
	}

	const fixed_point_t value = calculate_value(market_instance);

	if (state_buys) {
		share = std::min(share, fixed_point_t::_1 - state_share);
		const fixed_point_t price = value * share;
		if (share <= fixed_point_t::_0 || country.get_spendable_cash() < price) {
			return false;
		}
		country.spend_cash(price);
		state.add_to_capitalist_fund(price);
		state_share += share;
		return true;
	}

	// The capitalists only buy a factory that makes a profit, with the savings they have
	share = std::min(share, state_share);
	if (share <= fixed_point_t::_0 || average_profit <= fixed_point_t::_0 || value <= fixed_point_t::_0) {
		return false;
	}
	const fixed_point_t affordable = std::min(share, state.get_capitalist_fund() / value);
	if (affordable < get_defines().min_affordable_share.fixed()) {
		return false;
	}
	country.add_cash(state.take_from_capitalist_fund(value * affordable));
	state_share -= affordable;
	return true;
}

void FactoryInstance::set_open(const bool new_open) {
	open = new_open;
	if (open) {
		unprofitable_days = 0;
		throttle = fixed_point_t::_1;
	}
}

void FactoryInstance::after_buy(void* actor, BuyResult const& buy_result) {
	FactoryInstance& factory = *static_cast<FactoryInstance*>(actor);

	// Each order of a factory is for a different good, so each callback only writes to its own input
	for (Input& input : factory.inputs) {
		if (input.good.index == buy_result.good_index) {
			input.bought_today += buy_result.quantity_bought;
			input.spent_today += buy_result.money_spent_total;
			// Imported inputs are charged the tariff of the country, like the purchases of pops (the budget may not
			// cover it entirely, top_up_budget keeps the budget from going negative)
			if (factory.tariff_collector != nullptr) {
				input.spent_today += factory.tariff_collector->apply_tariff(buy_result.money_spent_on_imports);
			}
			return;
		}
	}
}

void FactoryInstance::after_sell(
	void* actor, SellResult const& sell_result, memory::vector<fixed_point_t>& reusable_vector
) {
	FactoryInstance& factory = *static_cast<FactoryInstance*>(actor);

	MoneyLedger::record(MoneyLedger::label_t::sell_callbacks_factories, sell_result.money_gained);
	factory.revenue_today += sell_result.money_gained;
	factory.sold_quantity_today += sell_result.quantity_sold;
}

void FactoryInstance::grant_starting_budget(MarketInstance const& market_instance, const building_level_t levels) {
	// The factory starts with a small budget so that a first unprofitable days do not close it at once
	budget += calculate_construction_cost(building_type, market_instance) * get_defines().starting_budget_share.fixed() *
		static_cast<int32_t>(type_safe::get(levels));
	MoneyLedger::record(
		MoneyLedger::label_t::factory_starting_budget,
		calculate_construction_cost(building_type, market_instance) * get_defines().starting_budget_share.fixed() *
			static_cast<int32_t>(type_safe::get(levels))
	);
}

void FactoryInstance::settle(State& state, MarketInstance const& market_instance) {
	// Goods bought yesterday
	fixed_point_t spent = 0;
	for (Input& input : inputs) {
		input.stockpile += input.bought_today;
		spent += input.spent_today;
		input.bought_today = 0;
		input.spent_today = 0;
	}

	const fixed_point_t revenue = revenue_today;
	sold_quantity_yesterday = sold_quantity_today;
	revenue_today = 0;
	sold_quantity_today = 0;

	budget += revenue;
	budget -= spent;
	MoneyLedger::record(MoneyLedger::label_t::factory_revenue, revenue);
	MoneyLedger::record(MoneyLedger::label_t::factory_spent, -spent);
	if (budget < 0) {
		// Money that the factory spent but did not have is created here
		MoneyLedger::record(MoneyLedger::label_t::factory_budget_floor, -budget);
		budget = 0;
	}

	const bool operated = produced_quantity_pending > 0 || revenue > 0;
	output_quantity_yesterday = produced_quantity_pending;
	input_cost_yesterday = consumed_value_pending;

	// Workers are paid half of the value they added, as long as the budget covers it. A factory that sells its goods
	// for less than its inputs cost does not pay wages, it is the market that decides if it keeps running.
	const fixed_point_t value_added = revenue - consumed_value_pending;
	fixed_point_t wages = value_added > 0 ? std::min(value_added * get_defines().wage_share, budget) : fixed_point_t { 0 };

	if (wages > 0 && !workers.empty()) {
		pop_size_t total_workers { 0 };
		for (Worker const& worker : workers) {
			total_workers += worker.size;
		}

		if (total_workers > pop_size_t(0)) {
			for (Worker const& worker : workers) {
				const fixed_point_t income = wages * type_safe::get(worker.size) / type_safe::get(total_workers);
				if (income > fixed_point_t::_0) {
					worker.pop->add_factory_worker_income(income);
				}
			}
		} else {
			wages = 0;
		}
	} else {
		wages = 0;
	}
	budget -= wages;
	MoneyLedger::record(MoneyLedger::label_t::factory_wages, -wages);

	// Distribution of the profit
	const fixed_point_t profit = revenue - consumed_value_pending - wages;
	fixed_point_t owner_income = 0;
	fixed_point_t state_income = 0;

	if (profit > 0) {
		// 1. A part of the profit goes into the expansion fund until it covers the cost of an expansion
		fixed_point_t to_fund = 0;
		if (can_be_expanded()) {
			const fixed_point_t cost = calculate_construction_cost(building_type, market_instance);
			to_fund = std::min(
				std::min(profit * get_defines().expansion_fund_share, std::max(cost - expansion_fund, fixed_point_t::_0)), budget
			);
			expansion_fund += to_fund;
			budget -= to_fund;
		}

		// 2. What the factory has beyond its working capital is paid to the owners (the state and the capitalists)
		const fixed_point_t working_capital = consumed_value_pending * get_defines().working_capital_days + get_defines().min_working_capital;
		const fixed_point_t excess = std::max(budget - working_capital, fixed_point_t::_0);
		const fixed_point_t dividend = std::min(excess, std::max(profit - to_fund, excess / get_defines().dividend_excess_divisor));
		if (dividend > 0) {
			pay_dividend(state, dividend, owner_income, state_income);
		}
	}

	subsidy_yesterday = subsidy_today;
	subsidy_today = fixed_point_t::_0;
	private_cover_yesterday = private_cover_today;
	private_cover_today = fixed_point_t::_0;
	revenue_yesterday = revenue;
	wages_yesterday = wages;
	owner_income_yesterday = owner_income;
	state_income_yesterday = state_income;
	profit_yesterday = profit;
	average_profit += (profit - average_profit) / get_defines().average_profit_days;

	produced_quantity_pending = 0;
	consumed_value_pending = 0;

	if (!operated) {
		return;
	}

	// Adapt to the market
	if (profit > 0) {
		profitable_days++;
		unprofitable_days = 0;
		efficiency = std::min(efficiency + fixed_point_t::epsilon * get_defines().efficiency_gain_epsilons, get_defines().efficiency_max);
		throttle = std::min(throttle + get_defines().throttle_gain, fixed_point_t::_1);
	} else {
		unprofitable_days++;
		profitable_days = 0;
		efficiency = std::max(efficiency - fixed_point_t::epsilon * get_defines().efficiency_loss_epsilons, get_defines().efficiency_min);
		if (subsidised) {
			// The state pays for the losses: the factory neither slows down nor closes
			throttle = fixed_point_t::_1;
			return;
		}
		throttle = std::max(throttle - get_defines().throttle_loss, get_defines().throttle_min);

		if (unprofitable_days >= static_cast<uint32_t>(get_defines().days_of_loss_before_closing) && throttle <= get_defines().throttle_min) {
			SPDLOG_INFO("Closing unprofitable factory {} in {}", building_type, state);
			open = false;
		}
	}
}

void FactoryInstance::advance_construction(State& state, const Date today, MarketInstance const& market_instance) {
	if (construction_end.has_value() && construction_uses_stockpile && today < *construction_end) {
		CountryInstance* const owner = state.get_owner();
		bool worked = false;

		if (owner != nullptr) {
			construction_pace += owner->get_construction_spending_slider_value().get_value_untracked();
			if (construction_pace >= fixed_point_t::_1) {
				// All the goods of the day have to be in the stockpile
				bool goods_available = true;
				for (auto const& [good, quantity] : building_type.goods_cost) {
					if (owner->get_good_data(market_instance.get_good_instance(good->index)).stockpile_amount
						< get_daily_construction_goods(*good)) {
						goods_available = false;
						break;
					}
				}

				if (goods_available) {
					for (auto const& [good, quantity] : building_type.goods_cost) {
						CountryInstance::good_data_t& good_data = owner->get_good_data(market_instance.get_good_instance(good->index));
						const fixed_point_t used = get_daily_construction_goods(*good);
						good_data.stockpile_amount -= used;
						good_data.stockpile_change_yesterday -= used;
					}
					construction_pace -= fixed_point_t::_1;
					worked = true;
				}
			}
		}

		if (!worked) {
			// Nothing was built today
			construction_end = *construction_end + Timespan::from_days(1);
		}
	}

	if (!construction_end.has_value() || today < *construction_end) {
		return;
	}

	construction_uses_stockpile = false;

	level += building_level_t(1);
	construction_start.reset();
	construction_end.reset();
	grant_starting_budget(market_instance, building_level_t(1));
	is_funded = true;
}

void FactoryInstance::pay_dividend(
	State& state, const fixed_point_t amount, fixed_point_t& capitalists_income, fixed_point_t& state_income
) {
	fixed_point_t capitalists_part = amount * (fixed_point_t::_1 - state_share);
	fixed_point_t state_part = amount - capitalists_part;

	if (capitalists_part > 0) {
		int32_t total_owner_size = 0;
		if (production_type.owner.has_value()) {
			for (Pop const& pop : state.get_pops_cache_by_type()[production_type.owner->pop_type_index]) {
				total_owner_size += type_safe::get(pop.get_size());
			}
		}

		if (total_owner_size > 0) {
			// Part of what the capitalists get is saved in the fund of the state, the rest is the income of their pops
			const fixed_point_t saved = capitalists_part * get_defines().capitalist_savings_share.fixed();
			const fixed_point_t paid_to_pops = capitalists_part - saved;
			state.add_to_capitalist_fund(saved);
			for (Pop& pop : state.get_pops_cache_by_type()[production_type.owner->pop_type_index]) {
				const fixed_point_t income = paid_to_pops * type_safe::get(pop.get_size()) / total_owner_size;
				if (income > fixed_point_t::_0) {
					pop.add_factory_owner_income(income);
				}
			}
		} else {
			// Without capitalists in the state the whole profit belongs to the state
			state_part += capitalists_part;
			capitalists_part = 0;
		}
	}

	if (state_part > 0) {
		CountryInstance* const country = state.get_owner();
		if (country != nullptr) {
			country->add_cash(state_part);
		} else {
			state_part = 0;
		}
	}

	budget -= capitalists_part + state_part;
	MoneyLedger::record(MoneyLedger::label_t::factory_dividends, -(capitalists_part + state_part));
	capitalists_income += capitalists_part;
	state_income += state_part;
}

void FactoryInstance::try_auto_expansion(const Date today, MarketInstance const& market_instance) {
	// Checked at the start of each month, like the monthly hiring of the workers
	if (!today.is_month_start() || !open || !can_be_expanded()) {
		return;
	}

	// Most of the jobs have to be filled, otherwise a bigger factory would not find workers
	const int32_t max_employees = type_safe::get(max_employee_count);
	if (max_employees <= 0 || static_cast<int64_t>(type_safe::get(employee_count)) * Rate::DENOMINATOR
			< static_cast<int64_t>(max_employees) * get_defines().expansion_min_employment.numerator) {
		return;
	}

	const fixed_point_t cost = calculate_construction_cost(building_type, market_instance);
	if (expansion_fund < cost) {
		return;
	}

	expansion_fund -= cost;
	MoneyLedger::record(MoneyLedger::label_t::expansion_fund_spent, -cost);
	profitable_days = 0;
	start_construction(today);
}

void FactoryInstance::hire(State& state) {
	workers.clear();
	employee_count = pop_size_t(0);
	max_employee_count = pop_size_t(0);

	if (level <= building_level_t(0)) {
		return;
	}

	const int32_t workforce = static_cast<int32_t>(type_safe::get(production_type.base_workforce_size)) *
		static_cast<int32_t>(type_safe::get(level));

	for (Job const& job : production_type.get_jobs()) {
		const int32_t max_for_job = (job.amount * workforce).floor<int32_t>();
		if (max_for_job <= 0) {
			continue;
		}

		max_employee_count += pop_size_t(static_cast<int32_t>(max_for_job));

		memory::vector<std::reference_wrapper<Pop>> const& pops = state.get_pops_cache_by_type()[job.pop_type_index];

		int32_t available = 0;
		for (Pop const& pop : pops) {
			available += type_safe::get(pop.get_unemployed());
		}
		if (available <= 0) {
			continue;
		}

		// Everyone is hired when there are enough jobs, otherwise jobs are shared proportionally between pops
		const fixed_point_t proportion = max_for_job >= available
			? fixed_point_t::_1
			: fp::from_fraction(max_for_job, available);

		for (Pop& pop : pops) {
			const int32_t unemployed = type_safe::get(pop.get_unemployed());
			const int32_t to_hire = (proportion * unemployed).floor<int32_t>();
			if (to_hire <= 0) {
				continue;
			}

			pop.hire(pop_size_t(to_hire));
			workers.push_back({ &pop, pop_size_t(to_hire) });
			employee_count += pop_size_t(to_hire);
		}
	}
}

void FactoryInstance::tick(State& state, MarketInstance& market_instance, const Date today) {
	settle(state, market_instance);
	advance_construction(state, today, market_instance);

	if (!is_funded && level > building_level_t(0)) {
		grant_starting_budget(market_instance, level);
		is_funded = true;

		// A factory in a state without capitalists belongs to the state, otherwise to the capitalists
		if (state_share < fixed_point_t::_1) {
			int32_t capitalists = 0;
			if (production_type.owner.has_value()) {
				for (Pop const& pop : state.get_pops_cache_by_type()[production_type.owner->pop_type_index]) {
					capitalists += type_safe::get(pop.get_size());
				}
			}
			if (capitalists <= 0) {
				state_share = fixed_point_t::_1;
				built_by_state = true;
			}
		}
	}

	workers.clear();
	employee_count = pop_size_t(0);

	CountryInstance* const owner = state.get_owner();
	if (owner == nullptr || level <= building_level_t(0) || !open) {
		return;
	}

	hire(state);

	try_auto_expansion(today, market_instance);

	const int32_t max_employees = type_safe::get(max_employee_count);
	const fixed_point_t workforce_fraction = max_employees > 0
		? fp::from_fraction(static_cast<int32_t>(type_safe::get(employee_count)), max_employees)
		: fixed_point_t { 0 };

	const int32_t level_value = static_cast<int32_t>(type_safe::get(level));

	// How much of the possible production the inputs in the stockpile allow
	fixed_point_t input_fraction = fixed_point_t::_1;
	for (Input const& input : inputs) {
		const fixed_point_t needed = input.quantity_per_level * level_value * workforce_fraction * throttle;
		if (needed > 0) {
			input_fraction = std::min(input_fraction, std::min(fixed_point_t::_1, input.stockpile / needed));
		}
	}

	const fixed_point_t production_fraction = workforce_fraction * throttle * input_fraction;

	fixed_point_t consumed_value = 0;
	if (production_fraction > 0) {
		for (Input& input : inputs) {
			const fixed_point_t consumed = std::min(
				input.quantity_per_level * level_value * production_fraction, input.stockpile
			);
			input.stockpile -= consumed;
			consumed_value += consumed * market_instance.get_good_instance(input.good.index).get_price();
		}
	}

	const std::optional<country_index_t> country_index = owner->index;
	tariff_collector = owner;
	// Part of the budget has to be kept for the tariff on imported inputs
	const fixed_point_t tariff_rate = owner->effective_tariff_rate.get_untracked();
	const fixed_point_t tariff_multiplier = tariff_rate > fixed_point_t::_0 ? fixed_point_t::_1 + tariff_rate : fixed_point_t::_1;

	const fixed_point_t output = production_type.base_output_quantity * level_value * production_fraction * efficiency;
	produced_quantity_pending = output;
	consumed_value_pending = consumed_value;

	if (output > 0) {
		// The callback is not called with a vector of its own as it does not need one
		memory::vector<fixed_point_t> reusable_vector;
		market_instance.place_market_sell_order(
			{ production_type.output_good.index, country_index, output, this, after_sell }, reusable_vector
		);
	}

	// The factory pays its purchases with its own budget first. What the budget lacks is covered by the owners:
	// the state (if the factory is subsidised, as far as the subsidy mode says) and the capitalists, from their
	// savings fund, for their share of the factory.
	{
		fixed_point_t desired = 0;
		for (Input const& input : inputs) {
			const fixed_point_t wanted = input.quantity_per_level * level_value * throttle * get_defines().stockpile_target_days - input.stockpile;
			if (wanted > 0 && market_instance.get_is_available(input.good.index)) {
				desired += market_instance.get_max_money_to_allocate_to_buy_quantity(input.good.index, wanted) * tariff_multiplier;
			}
		}
		const fixed_point_t shortfall = desired - budget;
		if (shortfall > 0) {
			fixed_point_t state_fraction = 0;
			fixed_point_t capitalist_fraction = fixed_point_t::_1 - state_share;
			if (subsidised) {
				switch (owner->get_subsidy_mode()) {
					using enum CountryInstance::subsidy_mode_t;
				case ALL:
					state_fraction = fixed_point_t::_1;
					capitalist_fraction = 0;
					break;
				case STATE_ONLY:
					// Only the factories the state owns a share of are subsidised, and the state pays all of their losses;
					// private factories get nothing from the state (the capitalists pay for them)
					if (state_share > fixed_point_t::_0) {
						state_fraction = fixed_point_t::_1;
						capitalist_fraction = fixed_point_t::_0;
					} else {
						state_fraction = fixed_point_t::_0;
					}
					break;
				case BY_SHARE:
					state_fraction = state_share;
					break;
				}
			}

			const fixed_point_t treasury = owner->get_spendable_cash();
			if (state_fraction > 0 && treasury > 0) {
				const fixed_point_t paid = std::min(shortfall * state_fraction, treasury);
				owner->spend_cash(paid);
				budget += paid;
				MoneyLedger::record(MoneyLedger::label_t::factory_subsidy, paid);
				subsidy_today += paid;
			}
			if (capitalist_fraction > 0) {
				const fixed_point_t paid = state.take_capitalist_money(
					shortfall * capitalist_fraction, Loan::purpose_t::LOSS_COVER, 0
				);
				budget += paid;
				MoneyLedger::record(MoneyLedger::label_t::factory_private_cover, paid);
				private_cover_today += paid;
			}
		}
	}

	// Buy the inputs for the next days, only spending what the factory's own budget can afford
	fixed_point_t budget_left = budget;
	for (Input& input : inputs) {
		const fixed_point_t target = input.quantity_per_level * level_value * throttle * get_defines().stockpile_target_days;
		const fixed_point_t wanted = target - input.stockpile;
		if (wanted <= 0 || budget_left <= 0 || !market_instance.get_is_available(input.good.index)) {
			continue;
		}

		const fixed_point_t money_to_spend = std::min(
			budget_left / tariff_multiplier, market_instance.get_max_money_to_allocate_to_buy_quantity(input.good.index, wanted)
		);
		if (money_to_spend <= 0) {
			continue;
		}

		budget_left -= money_to_spend * tariff_multiplier;
		market_instance.place_buy_up_to_order(
			{ input.good.index, country_index, wanted, money_to_spend, this, after_buy }
		);
	}
}

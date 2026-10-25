#pragma once

namespace OpenVic {
	struct CountryInstance;
	struct InstanceManager;

	/* The use of the investment fund of a country (CountryInstance::get_investment_fund). The construction slider of the budget
	 * sets aside a share of the income every day. Once a week (the day depends on the country) the planner reviews the
	 * enterprises and the railroads that the country could get:
	 *  - the cost of a project is the cost of building it at the current prices (the money and the goods);
	 *  - the profit of an enterprise is what its level earns in OpenVicEconomyDefines::investment_profit_days (half a year): the money
	 *    from the sale of the goods it makes minus the raw materials it uses, at the prices of the market; the profit of a railroad is
	 *    a share of the revenue of the resource gathering of its province;
	 *  - a project is approved when its profit is at least investment_min_return (10 percent) of its cost and the fund has the money.
	 * The money of an approved project is given to the pops of one type, chosen by chance from the pop types of the state with the
	 * weights of the defines (capitalists 50, aristocrats 25, artisans, clergymen, clerks, officers and bureaucrats 5 each); they
	 * build the enterprise and own it (they get its dividends). The chance is a hash of the date and the place: the simulation is
	 * deterministic. */
	struct InvestmentPlanner {
		// Called every day after the countries were ticked; reviews the countries whose day it is
		static void tick(InstanceManager& instance_manager);

	private:
		static void review(InstanceManager& instance_manager, CountryInstance& country);
	};
}

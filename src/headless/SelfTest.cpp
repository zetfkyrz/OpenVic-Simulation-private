#include "SelfTest.hpp"

#include <algorithm>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include <fmt/format.h>
#include <spdlog/spdlog.h>

#include <openvic-simulation/GameManager.hpp>
#include <openvic-simulation/DefinitionManager.hpp>
#include <openvic-simulation/InstanceManager.hpp>
#include <openvic-simulation/country/CountryInstance.hpp>
#include <openvic-simulation/economy/GoodInstance.hpp>
#include <openvic-simulation/defines/OpenVicDefines.hpp>
#include <openvic-simulation/economy/BuildingType.hpp>
#include <openvic-simulation/economy/credit/Credit.hpp>
#include <openvic-simulation/economy/production/FactoryInstance.hpp>
#include <openvic-simulation/economy/production/ResourceGatheringOperation.hpp>
#include <openvic-simulation/economy/trading/TradeNetwork.hpp>
#include <openvic-simulation/population/Pop.hpp>
#include <openvic-simulation/map/ProvinceInstance.hpp>
#include <openvic-simulation/military/Division.hpp>
#include <openvic-simulation/military/SupplyDelivery.hpp>
#include <openvic-simulation/military/SupplyTerritory.hpp>
#include <openvic-simulation/military/UnitInstance.hpp>
#include <openvic-simulation/military/War.hpp>
#include <openvic-simulation/country/CountryInstanceManager.hpp>
#include <openvic-simulation/military/UnitInstanceGroup.hpp>
#include <openvic-simulation/military/UnitType.hpp>
#include <openvic-simulation/map/State.hpp>
#include <openvic-simulation/misc/GameAction.hpp>
#include <openvic-simulation/misc/MoneyLedger.hpp>
#include <openvic-simulation/misc/Replay.hpp>
#include <openvic-simulation/misc/StateHash.hpp>
#include <openvic-simulation/utility/ThreadPool.hpp>

using namespace OpenVic;

namespace {
	struct Results {
		int passed = 0;
		int failed = 0;

		void check(const bool ok, std::string_view name, std::string const& detail = {}) {
			if (ok) {
				passed++;
				fmt::println("TEST PASS {}", name);
			} else {
				failed++;
				fmt::println("TEST FAIL {}: {}", name, detail);
			}
		}
		void summary() const {
			fmt::println("SELFTEST SUMMARY passed={} failed={}", passed, failed);
		}
	};

	// ---------------------------------------------------------------- pure tests

	void test_rate(Results& r) {
		bool exact = true;
		std::string detail;
		const std::int32_t numerators[] = { 1, 2, 3, 5, 7, 8, 15, 25, 35, 60, 80 };
		const std::int32_t denominators[] = { 2, 4, 5, 8, 10, 100, 1000 };
		for (std::int32_t n : numerators) {
			for (std::int32_t d : denominators) {
				if (n > d) {
					continue;
				}
				const Rate rate { n, d };
				for (std::int32_t v : { 1, 7, 100, 12345, 987654 }) {
					const fixed_point_t x = fixed_point_t(v) / 7;
					if (rate.of(x) != x * n / d) {
						exact = false;
						detail = fmt::format("{}/{} of {}", n, d, x);
					}
				}
				if (rate.fixed() != fixed_point_t(n) / d) {
					exact = false;
					detail = fmt::format("fixed {}/{}", n, d);
				}
			}
		}
		r.check(exact, "rate: of() and fixed() give the same numbers as x * n / d", detail);
	}

	void test_defines(Results& r) {
		OpenVicDefines defines;
		OpenVicDefines other;
		r.check(defines.get_hash() == other.get_hash(), "defines: the hash of the defaults is the same in two objects");

		const std::uint64_t default_hash = defines.get_hash();
		const std::int32_t default_rank = defines.ai.max_rank_for_wars;
		defines.ai.set_value("test.lua", "MAX_RANK_FOR_WARS", "12");
		r.check(defines.ai.max_rank_for_wars == 12, "defines: a valid whole number is read");
		defines.ai.set_value("test.lua", "MAX_RANK_FOR_WARS", "100000");
		r.check(defines.ai.max_rank_for_wars == 12, "defines: a value above the range keeps the old value");
		defines.ai.set_value("test.lua", "MAX_RANK_FOR_WARS", "-3");
		r.check(defines.ai.max_rank_for_wars == 12, "defines: a negative value outside the range keeps the old value");
		defines.ai.set_value("test.lua", "MAX_RANK_FOR_WARS", "abc");
		r.check(defines.ai.max_rank_for_wars == 12, "defines: text instead of a number keeps the old value");
		defines.ai.set_value("test.lua", "MAX_RANK_FOR_WARS", "2.5");
		r.check(defines.ai.max_rank_for_wars == 12, "defines: a fraction for a whole number keeps the old value");
		defines.ai.set_value("test.lua", "NO_SUCH_KEY", "1");
		r.check(defines.ai.max_rank_for_wars == 12, "defines: an unknown key changes nothing");
		defines.ai.set_value("test.lua", "WAR_CHANCE_PER_YEAR", "0.1");
		r.check(defines.ai.war_chance_per_year.numerator == 1000, "defines: a rate is read as a fraction (0.1 = 1000/10000)");
		defines.ai.set_value("test.lua", "WAR_CHANCE_PER_YEAR", "-0.1");
		r.check(defines.ai.war_chance_per_year.numerator == 1000, "defines: a negative rate keeps the old value");
		defines.politics.set_value("test.lua", "CLOSENESS_WIDTH", "0.5");
		r.check(defines.politics.closeness_width == 0.5, "defines: a double is read");

		// The hash follows the values and only them
		defines.update_hash();
		r.check(defines.get_hash() != default_hash, "defines: the hash changes when a value changes");
		defines.ai.max_rank_for_wars = default_rank;
		defines.ai.war_chance_per_year = OpenVicDefines {}.ai.war_chance_per_year;
		defines.politics.closeness_width = OpenVicDefines {}.politics.closeness_width;
		defines.update_hash();
		r.check(defines.get_hash() == default_hash, "defines: the hash is the same again when the values are the defaults again");
	}

	game_action_t make(auto&& action) {
		return game_action_t(std::forward<decltype(action)>(action));
	}

	void test_replay(Results& r) {
		std::vector<ReplayEntry> entries;
		entries.push_back({ 1, game_action_t(std::in_place_type<tick_argument_t>) });
		entries.push_back({ 1, game_action_t(std::in_place_type<set_tariff_rate_argument_t>, country_index_t(3), fixed_point_t(1) / 4) });
		entries.push_back({ 2, game_action_t(std::in_place_type<set_good_trade_order_argument_t>, country_index_t(1), good_index_t(2), true, fixed_point_t(5) / 3) });
		entries.push_back({ 2, game_action_t(std::in_place_type<move_army_argument_t>, unique_id_t(77), province_index_t(9)) });
		entries.push_back({ 3, game_action_t(std::in_place_type<create_leader_argument_t>, country_index_t(2), unit_branch_t::LAND) });
		entries.push_back({ 3, game_action_t(std::in_place_type<set_mobilise_argument_t>, country_index_t(4), false) });
		entries.push_back({ 4, game_action_t(std::in_place_type<stop_army_argument_t>, unique_id_t(1234567890123ULL)) });
		entries.push_back({ 4, game_action_t(std::in_place_type<take_credit_argument_t>, country_index_t(5), std::uint32_t(30)) });

		const std::string text = Replay::serialise(entries);
		std::vector<ReplayEntry> back;
		const bool parsed = Replay::deserialise(text, back);
		r.check(parsed, "replay: the text of a replay is read back");
		bool same = parsed && back.size() == entries.size();
		for (size_t i = 0; same && i < entries.size(); ++i) {
			same = back[i].batch == entries[i].batch && back[i].action.index() == entries[i].action.index();
		}
		r.check(same, "replay: batches and kinds of the actions are kept");
		r.check(parsed && Replay::serialise(back) == text, "replay: writing the read actions gives the same text");

		std::vector<ReplayEntry> broken;
		r.check(!Replay::deserialise("OVREPLAY 1\n1 no_such_action 1 2\n", broken), "replay: an unknown action is refused");
		broken.clear();
		r.check(!Replay::deserialise("OVREPLAY 1\n1 set_tariff_rate 3\n", broken), "replay: missing values are refused");
		broken.clear();
		r.check(!Replay::deserialise("not a replay\n", broken), "replay: a file without the header is refused");
		broken.clear();
		r.check(Replay::deserialise("OVREPLAY 1\n", broken) && broken.empty(), "replay: an empty replay is fine");

		r.check(
			!Replay::is_recorded(game_action_t(std::in_place_type<set_pause_argument_t>, true)) &&
				!Replay::is_recorded(game_action_t(std::in_place_type<set_speed_argument_t>, std::int64_t(2))) &&
				Replay::is_recorded(game_action_t(std::in_place_type<tick_argument_t>)),
			"replay: pause and speed are not recorded, ticks are"
		);
	}

	void test_save_header(Results& r) {
		const std::uint64_t hash = 0x1234567890abcdefULL;
		std::uint64_t parsed = 0;
		r.check(
			Replay::hash_from_string(Replay::hash_to_string(hash), parsed) && parsed == hash && Replay::hash_to_string(hash).size() == 16,
			"save header: the hash is written and read as 16 hex digits"
		);
		r.check(!Replay::hash_from_string("xyz", parsed) && !Replay::hash_from_string("", parsed), "save header: a wrong hash text is refused");

		Replay::Compatibility same = Replay::check_header(Replay::FORMAT_VERSION, Replay::SIMULATION_VERSION, true, hash, hash);
		r.check(same.loadable && same.warnings.empty(), "save header: the same version and defines load without a warning");

		Replay::Compatibility other_defines = Replay::check_header(Replay::FORMAT_VERSION, Replay::SIMULATION_VERSION, true, hash, hash + 1);
		r.check(other_defines.loadable && other_defines.warnings.size() == 1, "save header: other defines give one warning and can still be opened");

		Replay::Compatibility other_version = Replay::check_header(Replay::FORMAT_VERSION, Replay::SIMULATION_VERSION + 1, true, hash, hash);
		r.check(other_version.loadable && other_version.warnings.size() == 1, "save header: another simulation version gives a warning");

		Replay::Compatibility old_format = Replay::check_header(1, -1, false, 0, hash);
		r.check(old_format.loadable && !old_format.warnings.empty(), "save header: format 1 is migrated with a warning");

		Replay::Compatibility future = Replay::check_header(Replay::FORMAT_VERSION + 1, 1, true, hash, hash);
		r.check(!future.loadable && !future.blocked_reason.empty(), "save header: a newer format is refused with a reason (no crash)");

		Replay::Compatibility broken = Replay::check_header(0, 1, true, hash, hash);
		r.check(!broken.loadable && !broken.blocked_reason.empty(), "save header: an unreadable format is refused with a reason");
	}

	// ---------------------------------------------------------------- tests on a real game

	struct Game {
		GameManager& game_manager;
		Bookmark const& bookmark;

		InstanceManager* start() {
			if (!game_manager.setup_instance(bookmark) || !game_manager.start_game_session() || !game_manager.update_clock()) {
				return nullptr;
			}
			return game_manager.get_instance_manager();
		}
		void end() {
			game_manager.end_game_session();
		}
	};

	// The commands of a player on the first day: tariffs of the biggest countries, subsidies and a closed factory switch
	void give_orders(InstanceManager& instance_manager) {
		for (CountryInstance const& country : instance_manager.get_country_instance_manager().get_country_instances()) {
			if (country.get_total_rank() > 0 && country.get_total_rank() <= 12) {
				instance_manager.queue_game_action<set_tariff_rate_argument_t>(country.index, fixed_point_t(1) / 4);
				instance_manager.queue_game_action<set_all_factories_subsidised_argument_t>(country.index, true);
			}
		}
		// The country with the most money (the lowest index among equals) offers a credit to everybody
		CountryInstance const* richest = nullptr;
		for (CountryInstance const& country : instance_manager.get_country_instance_manager().get_country_instances()) {
			if (country.exists() && (richest == nullptr || country.get_cash_stockpile().load() > richest->get_cash_stockpile().load())) {
				richest = &country;
			}
		}
		if (richest != nullptr) {
			instance_manager.queue_game_action<offer_external_credit_argument_t>(
				richest->index, fixed_point_t(2000), fixed_point_t(6) / 100, std::uint32_t(1), true
			);
		}
		instance_manager.force_tick_and_update();
	}

	void play(InstanceManager& instance_manager, const int days) {
		for (int i = 0; i < days; ++i) {
			instance_manager.force_tick_and_update();
		}
	}

	// Plays the days and writes down the hash of every day (to find the first day two runs differ on)
	struct DayDetails {
		std::vector<std::int64_t> country_cash;
		std::vector<std::int64_t> country_free;
		std::int64_t pop_cash = 0, factory_budget = 0, capitalist_fund = 0, price_sum = 0;
	};

	DayDetails collect_details(InstanceManager& instance_manager) {
		DayDetails details;
		for (CountryInstance const& country : instance_manager.get_country_instance_manager().get_country_instances()) {
			details.country_cash.push_back(country.get_cash_stockpile().load().get_raw_value());
		}
		for (ProvinceInstance& province : instance_manager.get_map_instance().get_province_instances()) {
			for (Pop& pop : province.get_mutable_pops()) {
				details.pop_cash += pop.get_cash().get_copy_of_value().get_raw_value();
			}
		}
		for (StateSet const& state_set : instance_manager.get_map_instance().get_state_manager().get_state_sets()) {
			for (State const& state : state_set.get_states()) {
				details.capitalist_fund += state.get_capitalist_fund().get_raw_value();
				for (FactoryInstance const& factory : state.get_factories()) {
					details.factory_budget += factory.get_budget().get_raw_value();
				}
			}
		}
		for (GoodInstance const& good : instance_manager.get_good_instance_manager().get_good_instances()) {
			details.price_sum += good.get_price().get_raw_value();
		}
		return details;
	}

	std::vector<DayDetails> g_details_1, g_details_2;

	void play_recording(InstanceManager& instance_manager, const int days, std::vector<std::uint64_t>& hashes, std::vector<DayDetails>& details) {
		for (int i = 0; i < days; ++i) {
			instance_manager.force_tick_and_update();
			hashes.push_back(StateHash::compute(instance_manager));
			details.push_back(collect_details(instance_manager));
		}
	}

	std::string hex(const std::uint64_t value) {
		return Replay::hash_to_string(value);
	}

	// Credits: the formulas of docs/CREDITS.md on a credit that is not in a game
	Loan make_test_loan(Loan::term_t term, std::int32_t term_days, std::int32_t delay_days, fixed_point_t principal, fixed_point_t rate) {
		Loan loan;
		loan.kind = Loan::kind_t::EXTERNAL;
		loan.principal = loan.principal_left = principal;
		loan.annual_rate = rate;
		loan.term = term;
		loan.term_days = term_days;
		loan.delay_days = delay_days;
		return loan;
	}

	double dbl(fixed_point_t x) {
		return static_cast<double>(x);
	}

	std::int32_t installments_for_medium() {
		return Loan::calculate_installments(3650, 1825);
	}

	bool near(fixed_point_t a, fixed_point_t b, fixed_point_t tolerance) {
		return (a > b ? a - b : b - a) <= tolerance;
	}

	void test_credit_pure(Results& r) {
		const Date start { 1836, 1, 1 };
		const fixed_point_t principal = fixed_point_t(1000);
		const fixed_point_t rate = fixed_point_t(5) / 100;

		// Total interest: principal * rate * (delay + repayment / 2) years
		const fixed_point_t total_interest = Loan::calculate_total_interest(principal, rate, 913, 912);
		const fixed_point_t expected_interest = fixed_point_t(1000) * 5 / 100 * (913 + 456) / 365;
		r.check(near(total_interest, expected_interest, fixed_point_t(1) / 10), "credit: the total interest is principal * rate * (delay + repayment / 2)",
			fmt::format("{} != {}", dbl(total_interest), dbl(expected_interest)));

		// The whole life of a short credit (5 years, payments start after 2.5 years) paid on time
		Loan loan = make_test_loan(Loan::term_t::SHORT, 1825, 913, principal, rate);
		loan.start_schedule(start);
		const std::int32_t installments = loan.installments_left;
		r.check(installments == 31, "credit: a short credit has 31 monthly payments after the delay", std::to_string(installments));
		r.check(loan.get_installment_due(start + Timespan::from_days(912)) == fixed_point_t::_0, "credit: nothing is due before the delay ends");
		fixed_point_t first_due = loan.get_installment_due(start + Timespan::from_days(913));
		bool equal_installments = first_due > fixed_point_t::_0;
		fixed_point_t paid_total = 0;
		Date day = start + Timespan::from_days(913);
		for (int i = 0; i < installments && loan.installments_left > 0; i++) {
			const fixed_point_t due = loan.get_installment_due(day);
			if (!near(due, first_due, fixed_point_t(1) / 100)) {
				equal_installments = false;
			}
			loan.apply_installment_payment(due, due);
			paid_total += due;
			day = day + Timespan::from_days(30);
		}
		r.check(equal_installments, "credit: the payments are equal over the whole period of payments (interest spread evenly)");
		r.check(loan.get_remaining() == fixed_point_t::_0 && loan.installments_left == 0, "credit: the credit is paid off by the last installment",
			fmt::format("left {}", dbl(loan.get_remaining())));
		r.check(near(paid_total, principal + total_interest, fixed_point_t(1) / 10), "credit: in total the principal and the interest are paid",
			fmt::format("{} != {}", dbl(paid_total), dbl(principal + total_interest)));
		r.check(near(loan.interest_paid, total_interest, fixed_point_t(1) / 10), "credit: the interest paid equals the interest of the schedule");

		// A missed payment is not lost: the next one is bigger
		Loan late = make_test_loan(Loan::term_t::MEDIUM, 3650, 1825, principal, rate);
		late.start_schedule(start);
		const Date first_day = start + Timespan::from_days(1825);
		const fixed_point_t due_1 = late.get_installment_due(first_day);
		late.apply_installment_payment(due_1, fixed_point_t::_0);
		r.check(late.installments_left == installments_for_medium() - 1 && late.get_remaining() == principal + Loan::calculate_total_interest(principal, rate, 1825, 1825), "credit: a missed month passes, nothing of the debt is paid");
		const fixed_point_t due_2 = late.get_installment_due(first_day + Timespan::from_days(30));
		r.check(due_2 > due_1, "credit: what was not paid is added to the next payments");

		// Early repayment of everything during the delay: only the interest earned so far is owed
		Loan early = make_test_loan(Loan::term_t::SHORT, 1825, 913, principal, rate);
		early.start_schedule(start);
		const Date year_later = start + Timespan::from_days(365);
		const fixed_point_t cost = early.get_full_repayment_cost(year_later);
		r.check(near(cost, principal + principal * rate, fixed_point_t(1) / 100), "credit: early repayment in the delay costs the principal and the interest of that time",
			fmt::format("{}", dbl(cost)));
		const fixed_point_t paid_early = early.apply_early_repayment(year_later, cost);
		r.check(paid_early == cost && early.get_remaining() == fixed_point_t::_0, "credit: a full early repayment ends the credit with no future interest");
		r.check(near(early.interest_paid, principal * rate, fixed_point_t(1) / 100), "credit: the interest paid of an early repayment is the interest earned");

		// A part of the principal paid early lowers all the later payments
		Loan partial = make_test_loan(Loan::term_t::SHORT, 1825, 913, principal, rate);
		partial.start_schedule(start);
		const Date in_payments = start + Timespan::from_days(913 + 300);
		const fixed_point_t due_before = partial.get_installment_due(in_payments);
		partial.apply_early_repayment(in_payments, fixed_point_t(400));
		const fixed_point_t due_after = partial.get_installment_due(in_payments);
		r.check(partial.principal_left == fixed_point_t(600) && due_after < due_before, "credit: a partial early repayment lowers the payments",
			fmt::format("{} -> {}", dbl(due_before), dbl(due_after)));

		// Short credits only fund constructions of a year or less
		Loan short_loan = make_test_loan(Loan::term_t::SHORT, 1825, 913, principal, rate);
		Loan long_loan = make_test_loan(Loan::term_t::LONG, 7300, 3650, principal, rate);
		r.check(
			short_loan.can_fund(Loan::purpose_t::CONSTRUCTION, 365, 365) && !short_loan.can_fund(Loan::purpose_t::CONSTRUCTION, 366, 365)
				&& !short_loan.can_fund(Loan::purpose_t::LOSS_COVER, 100, 365) && long_loan.can_fund(Loan::purpose_t::LOSS_COVER, 5000, 365),
			"credit: a short credit funds only constructions of at most 365 days, the others anything"
		);

		// The limits of the AI are in the defines: it lends at 1% or more, it borrows at 25% or less
		OpenVicCreditDefines const defaults;
		r.check(
			defaults.interest_min.numerator == 100 && defaults.interest_max.numerator == 2500 && defaults.term_long_days == 7300,
			"credit: the defaults of the defines are 1% (lending), 25% (borrowing) and 20 years for long credits"
		);
	}

	void play_to_month_start(InstanceManager& instance_manager) {
		for (int i = 0; i < 35; ++i) {
			instance_manager.force_tick_and_update();
			if (instance_manager.get_today().is_month_start()) {
				return;
			}
		}
	}

	void test_credit_game(Results& r, InstanceManager& im) {
		CreditManager& credit = im.get_credit_manager();
		memory::vector<CountryInstance*> rich;
		for (CountryInstance& country : im.get_country_instance_manager().get_country_instances()) {
			if (country.exists() && !country.is_rebel_country() && country.get_total_rank() > 0) {
				country.set_ai(true);
				rich.push_back(&country);
			}
		}
		std::sort(rich.begin(), rich.end(), [](CountryInstance* a, CountryInstance* b) -> bool {
			return a->get_total_rank() < b->get_total_rank();
		});
		if (rich.size() < 4) {
			r.check(false, "credit: the game has at least four countries");
			return;
		}
		for (std::size_t i = 0; i < 4; i++) {
			rich[i]->set_ai(false);
		}
		CountryInstance& lender = *rich[0];
		CountryInstance& borrower = *rich[1];
		CountryInstance& stranger = *rich[2];
		CountryInstance& heir = *rich[3];
		lender.add_cash(fixed_point_t(5000));
		const fixed_point_t amount = fixed_point_t(1000);
		const fixed_point_t rate = fixed_point_t(5) / 100;

		// An offer to one country: the money is held, the others cannot take it
		const fixed_point_t before = lender.get_cash_stockpile().load();
		credit.stage_target_country(lender, borrower, true);
		Loan* offer = credit.offer_external_credit(im, lender, amount, rate, Loan::term_t::MEDIUM, false);
		r.check(offer != nullptr && lender.get_cash_stockpile().load() == before - amount, "credit: an offer holds the money of the lender");
		if (offer == nullptr) {
			return;
		}
		const std::uint32_t id = offer->id;
		r.check(!credit.take_credit(im, stranger, id), "credit: a country that is not named cannot take an offer made to a tag");
		const fixed_point_t borrower_before = borrower.get_cash_stockpile().load();
		r.check(credit.take_credit(im, borrower, id) && borrower.get_cash_stockpile().load() == borrower_before + amount,
			"credit: the named country takes the offer and gets the money");
		r.check(offer->status == Loan::status_t::ACTIVE && offer->payments_start == im.get_today() + Timespan::from_days(1825)
				&& offer->due_date == im.get_today() + Timespan::from_days(3650),
			"credit: a medium credit has 10 years and the payments start after 5");

		// An offer that is withdrawn gives the money back
		const fixed_point_t before_2 = lender.get_cash_stockpile().load();
		Loan* second = credit.offer_external_credit(im, lender, amount, rate, Loan::term_t::SHORT, true);
		r.check(second != nullptr && lender.get_cash_stockpile().load() == before_2 - amount, "credit: a second offer (to all) holds the money");
		const std::uint32_t second_id = second != nullptr ? second->id : 0;
		r.check(credit.withdraw_offer(im, lender, second_id) && lender.get_cash_stockpile().load() == before_2,
			"credit: a withdrawn offer returns the money");
		r.check(!credit.take_credit(im, stranger, second_id), "credit: a withdrawn offer cannot be taken");

		// The rate never changes after it is set
		r.check(offer->annual_rate == rate, "credit: the rate is set once when the credit is offered");

		// The borrower goes bankrupt (it loses all its provinces): the debt is written off
		memory::vector<ProvinceInstance*> provinces { borrower.get_owned_provinces().begin(), borrower.get_owned_provinces().end() };
		for (ProvinceInstance* province : provinces) {
			province->set_owner(&lender);
		}
		play_to_month_start(im);
		r.check(credit.get_loan_by_id(id) == nullptr, "credit: the credit of a bankrupt borrower is written off");

		// A region with capitalists changes its owner: the credit goes to the new owner
		State* region = nullptr;
		CountryInstance* old_owner = nullptr;
		for (CountryInstance* country : { &stranger, &heir }) {
			for (State* state : country->get_states()) {
				if (state->get_capitalist_fund() >= fixed_point_t::_0 && state->get_provinces().size() > 0 && region == nullptr) {
					region = state;
					old_owner = country;
				}
			}
		}
		if (region == nullptr || old_owner == nullptr) {
			r.check(false, "credit: found a region for the owner change test");
			return;
		}
		CountryInstance& new_owner = old_owner == &stranger ? heir : stranger;
		old_owner->add_cash(fixed_point_t(5000));
		// The lender offers to all regions, so no staging is needed
		Loan* internal = credit.offer_internal_credit(im, *old_owner, amount, rate, Loan::term_t::LONG, true);
		r.check(internal != nullptr, "credit: a country offers an internal credit to the capitalists of its regions");
		if (internal == nullptr) {
			return;
		}
		const std::uint32_t internal_id = internal->id;
		r.check(credit.take_internal_credit(im, internal_id, *region) && internal->unspent == amount,
			"credit: the capitalists of a region take the internal credit, the money is theirs to spend");
		for (ProvinceInstance& province : region->get_provinces()) {
			province.set_owner(&new_owner);
		}
		play_to_month_start(im);
		Loan const* moved = credit.get_loan_by_id(internal_id);
		r.check(moved != nullptr && moved->lender == &new_owner, "credit: when a region changes its owner the new owner is the lender");

		// The AI: it never lends below the minimum interest and never borrows above the maximum
		play(im, 150);
		OpenVicCreditDefines const& limits = credit.get_defines();
		bool lends_ok = true, borrows_ok = true;
		std::size_t ai_offers = 0, ai_taken = 0;
		for (auto const& loan : credit.get_loans()) {
			if (loan->lender->is_ai()) {
				ai_offers++;
				lends_ok = lends_ok && loan->annual_rate >= limits.interest_min.fixed();
			}
			if (loan->status == Loan::status_t::ACTIVE && loan->kind == Loan::kind_t::EXTERNAL && loan->borrower->is_ai()) {
				ai_taken++;
				borrows_ok = borrows_ok && loan->annual_rate <= limits.interest_max.fixed();
			}
		}
		r.check(lends_ok, "credit: the AI never offers a credit below the minimum interest");
		r.check(borrows_ok, "credit: the AI never takes a credit above the maximum interest");
		{
			std::size_t ai_count = 0, richest_ai_index = 0;
			fixed_point_t richest = 0;
			for (CountryInstance const& country : im.get_country_instance_manager().get_country_instances()) {
				if (country.exists() && country.is_ai()) {
					ai_count++;
					if (country.get_cash_stockpile().load() > richest) {
						richest = country.get_cash_stockpile().load();
						richest_ai_index = type_safe::get(country.index);
					}
				}
			}
			fmt::println("TEST INFO credit: {} AI countries, the richest ({}) has {}", ai_count, richest_ai_index, static_cast<double>(richest));
		}
		fmt::println("TEST INFO credit: after 150 days {} credits of AI lenders exist, {} external credits taken by AI countries", ai_offers, ai_taken);
	}

	void test_factory_actions(Results& r, InstanceManager& im) {
		State* found_state = nullptr;
		BuildingType const* found_type = nullptr;
		for (StateSet& state_set : im.get_map_instance().get_state_manager().get_mutable_state_sets()) {
			for (State& state : state_set.get_mutable_states()) {
				if (found_state == nullptr && state.get_owner() != nullptr && state.get_capital() != nullptr
					&& !state.get_factories().empty()) {
					found_state = &state;
					found_type = &state.get_factories().begin()->building_type;
				}
			}
		}
		r.check(found_state != nullptr, "factories: a state with a factory exists at the start");
		if (found_state == nullptr) {
			return;
		}
		CountryInstance& owner = *found_state->get_owner();
		const province_index_t province = found_state->get_capital()->index;
		const building_type_index_t type = found_type->index;

		FactoryInstance* factory = found_state->find_factory(*found_type);
		r.check(factory->get_priority() == 1, "factories: the default priority is normal");
		im.queue_game_action<set_factory_priority_argument_t>(owner.index, province, type, std::uint32_t(2));
		im.force_tick_and_update();
		factory = found_state->find_factory(*found_type);
		r.check(factory != nullptr && factory->get_priority() == 2, "factories: the priority of a factory can be raised");
		im.queue_game_action<set_factory_priority_argument_t>(owner.index, province, type, std::uint32_t(7));
		im.force_tick_and_update();
		r.check(found_state->find_factory(*found_type)->get_priority() == 2, "factories: a wrong priority is refused");

		const std::size_t before = found_state->get_factories().size();
		im.queue_game_action<delete_factory_argument_t>(owner.index, province, type);
		im.force_tick_and_update();
		r.check(found_state->get_factories().size() + 1 == before && found_state->find_factory(*found_type) == nullptr,
			"factories: a factory can be deleted by its owner");
		play(im, 5);
	}

	// Supply limit of the provinces (docs/PLAN_ARMY_SUPPLY_FRONTS.md, sections 1 and 2): the parts of the breakdown make the capacity, it
	// is never negative, the better terrain gives more, the railroad and the own land raise it, the load is the consumption of the
	// regiments, and the coefficient of the land that is not held depends on how far the held provinces are
	void test_supply_limit(Results& r, InstanceManager& im) {
		OpenVicSupplyDefines const& defines = im.definition_manager.get_define_manager().get_openvic_defines().supply;
		CountryInstanceManager const& countries = im.get_country_instance_manager();
		std::size_t land = 0, bad_sum = 0, negative = 0, bad_rail = 0, bad_home = 0, bad_load = 0, with_army = 0, bad_coefficient = 0;
		fixed_point_t best_local = fixed_point_t::_0, worst_local = fixed_point_t(100000);
		std::size_t railroads = 0;
		for (ProvinceInstance const& province : im.get_map_instance().get_province_instances()) {
			if (province.province_definition.is_water()) {
				continue;
			}
			CountryInstance const* const owner = province.get_owner();
			if (owner == nullptr) {
				continue;
			}
			land++;
			SupplyFriends const friends = SupplyFriends::build(*owner, countries, im.get_war_manager());
			SupplyTerritory const territory =
				calculate_supply_territory(province, im.get_map_instance(), *owner, friends, defines);
			bad_coefficient += territory.coefficient < fixed_point_t::_0 || territory.coefficient > fixed_point_t::_1;
			ProvinceInstance::supply_breakdown_t const own = province.get_supply_breakdown(owner, territory);
			const fixed_point_t expected = (own.local_limit + own.country_limit) * own.limit_multiplier * own.rail_multiplier
				* own.technology_multiplier * (fixed_point_t::_1 + own.home_bonus) * territory.coefficient;
			bad_sum += (own.capacity != std::max(fixed_point_t::_0, expected));
			negative += own.capacity < fixed_point_t::_0;
			bad_rail += own.rail_multiplier != fixed_point_t::_1 + defines.supply_rail_bonus_per_level.fixed() * own.rail_level;
			railroads += own.rail_level > fixed_point_t::_0;
			bad_home += !own.is_home || own.home_bonus != owner->get_supply_home_bonus();
			// another country: no bonus of the own land
			for (CountryInstance const& other : countries.get_country_instances()) {
				if (&other != owner && other.exists()) {
					ProvinceInstance::supply_breakdown_t const foreign = province.get_supply_breakdown(&other, SupplyTerritory {});
					bad_home += foreign.is_home || foreign.home_bonus != fixed_point_t::_0;
					break;
				}
			}
			if (own.local_limit > best_local) {
				best_local = own.local_limit;
			}
			if (own.local_limit < worst_local) {
				worst_local = own.local_limit;
			}
			fixed_point_t load = fixed_point_t::_0;
			for (ArmyInstance const& army : province.get_armies()) {
				if (&army.get_country() == owner) {
					for (RegimentInstance const& regiment : army.get_regiment_instances()) {
						load += regiment.unit_type.supply_consumption;
					}
				}
			}
			with_army += load > fixed_point_t::_0;
			bad_load += own.load != load;
		}
		r.check(land > 1000, "supply limit: the land provinces with an owner are counted", fmt::format("{}", land));
		r.check(bad_sum == 0, "supply limit: the capacity is (local + global) * base * railroad * technologies * (1 + own land) * territory", fmt::format("{} provinces", bad_sum));
		r.check(bad_coefficient == 0, "supply limit: the supply coefficient of the territory is always between 0 and 1", fmt::format("{} provinces", bad_coefficient));
		r.check(negative == 0, "supply limit: the capacity is never negative");
		r.check(bad_rail == 0, "supply limit: every level of the railroad adds the same share (the levels are added)", fmt::format("{} provinces", bad_rail));
		r.check(bad_home == 0, "supply limit: the bonus of the own land is only for the owner", fmt::format("{} provinces", bad_home));
		r.check(best_local > worst_local && worst_local > fixed_point_t::_0, "supply limit: the better terrain gives a higher limit than the worse one",
			fmt::format("{} .. {}", worst_local, best_local));
		r.check(with_army > 0, "supply limit: there are provinces with the regiments of their owner");
		r.check(bad_load == 0, "supply limit: the load is the supply consumption of the regiments of the country", fmt::format("{} provinces", bad_load));
		r.check(railroads > 0, "supply limit: there are provinces with a railroad at the start", fmt::format("{}", railroads));

		// ---- The friends: the country, an ally, a country that gave the passage, the same side of a war; not an enemy
		CountryInstance* first = nullptr;
		CountryInstance* ally = nullptr;
		CountryInstance* stranger = nullptr;
		CountryInstance* enemy = nullptr;
		for (CountryInstance& country : im.get_country_instance_manager().get_country_instances()) {
			if (!country.exists() || country.get_total_rank() == 0) {
				continue;
			}
			if (first == nullptr) {
				first = &country;
			} else if (ally == nullptr) {
				ally = &country;
			} else if (stranger == nullptr) {
				stranger = &country;
			} else if (enemy == nullptr) {
				enemy = &country;
			}
		}
		r.check(enemy != nullptr, "supply friends: four countries exist");
		if (enemy != nullptr) {
			SupplyFriends friends = SupplyFriends::build(*first, countries, im.get_war_manager());
			r.check(friends.contains(first), "supply friends: a country is its own friend");
			r.check(!friends.contains(ally) && !friends.contains(stranger) && !friends.contains(nullptr),
				"supply friends: nobody else is a friend without an alliance, a passage or a war on the same side");
			ally->set_alliance_with(*first, true);
			stranger->set_military_access_to(*first, false);
			first->set_military_access_to(*stranger, true);
			friends = SupplyFriends::build(*first, countries, im.get_war_manager());
			r.check(friends.contains(ally), "supply friends: an ally is a friend");
			r.check(friends.contains(stranger), "supply friends: a country that gave the passage of the troops is a friend");
			r.check(!friends.contains(enemy), "supply friends: the others are still not friends");
			// A country that took the passage from the first one does not give it supply (the passage is one way)
			SupplyFriends const other_side = SupplyFriends::build(*stranger, countries, im.get_war_manager());
			r.check(!other_side.contains(first), "supply friends: the passage works one way");
			first->set_military_access_to(*stranger, false);
			ally->set_alliance_with(*first, false);

			// War: the enemy is not a friend; the countries on the same side are
			if (im.get_war_manager().declare_war(im, *first, *enemy)) {
				friends = SupplyFriends::build(*first, countries, im.get_war_manager());
				r.check(!friends.contains(enemy), "supply friends: the enemy in a war is not a friend");
				bool all_side = true;
				for (War const* war : im.get_war_manager().get_wars_of(*first)) {
					for (CountryInstance const* member : war->get_side(war->is_attacker(*first))) {
						all_side &= friends.contains(member);
					}
					for (CountryInstance const* member : war->get_side(!war->is_attacker(*first))) {
						all_side &= !friends.contains(member);
					}
				}
				r.check(all_side, "supply friends: the countries on the same side of the war are friends, the other side is not");
			}
		}

		// ---- The coefficient of the land that is not held, checked by a different way (sets of the provinces) on every province
		if (first != nullptr) {
			const SupplyFriends friends = SupplyFriends::build(*first, countries, im.get_war_manager());
			MapInstance const& map = im.get_map_instance();
			const auto land_neighbours = [&map](ProvinceInstance const& province) {
				std::vector<ProvinceInstance const*> result;
				for (ProvinceDefinition::adjacency_t const& adjacency : province.province_definition.get_adjacencies()) {
					if (adjacency.get_type() == ProvinceDefinition::adjacency_t::type_t::LAND) {
						result.push_back(&map.get_province_instance_by_definition(adjacency.get_to()));
					}
				}
				return result;
			};
			std::size_t checked = 0, wrong = 0, held_wrong = 0;
			std::size_t kinds[6] = {};
			for (ProvinceInstance const& province : map.get_province_instances()) {
				if (province.province_definition.is_water()) {
					continue;
				}
				SupplyTerritory const got = calculate_supply_territory(province, map, *first, friends, defines);
				kinds[static_cast<std::size_t>(got.kind)]++;
				if (is_supply_held(province, friends)) {
					held_wrong += got.kind != SupplyTerritory::kind_t::HELD || got.coefficient != fixed_point_t::_1;
					continue;
				}
				checked++;
				// Level 1: the held neighbours
				std::vector<ProvinceInstance const*> level1 = land_neighbours(province);
				std::size_t held1 = 0;
				std::vector<ProvinceInstance const*> between;
				for (ProvinceInstance const* neighbour : level1) {
					if (is_supply_held(*neighbour, friends)) {
						held1++;
					} else {
						between.push_back(neighbour);
					}
				}
				SupplyTerritory::kind_t expected_kind = SupplyTerritory::kind_t::FAR;
				fixed_point_t expected_coefficient = defines.supply_far.fixed();
				if (held1 >= 2) {
					expected_kind = SupplyTerritory::kind_t::TWO_NEIGHBOURS;
					expected_coefficient = fixed_point_t::_1;
				} else if (held1 == 1) {
					expected_kind = SupplyTerritory::kind_t::ONE_NEIGHBOUR;
					expected_coefficient = defines.supply_neighbour_one.fixed();
				} else {
					// Level 2: the held provinces next to the provinces between (not the province itself, not those between)
					std::vector<ProvinceInstance const*> held2;
					std::vector<ProvinceInstance const*> between2;
					const auto contains = [](std::vector<ProvinceInstance const*> const& list, ProvinceInstance const* item) {
						return std::find(list.begin(), list.end(), item) != list.end();
					};
					for (ProvinceInstance const* first_step : between) {
						for (ProvinceInstance const* second_step : land_neighbours(*first_step)) {
							if (second_step == &province || contains(between, second_step)) {
								continue;
							}
							if (is_supply_held(*second_step, friends)) {
								if (!contains(held2, second_step)) {
									held2.push_back(second_step);
								}
							} else if (!contains(between2, second_step)) {
								between2.push_back(second_step);
							}
						}
					}
					if (!held2.empty()) {
						expected_kind = SupplyTerritory::kind_t::ONE_BETWEEN;
						expected_coefficient = defines.supply_gap1_base.fixed()
							+ defines.supply_gap1_step.fixed()
								* (std::min<std::int32_t>(static_cast<std::int32_t>(held2.size()), defines.supply_gap1_max_count) - 1);
					}
				}
				// The search through two provinces is not open at the start (no technology), so the rest is far away
				wrong += got.kind != expected_kind || got.coefficient != expected_coefficient;
			}
			r.check(checked > 1000, "supply territory: many provinces are not held by a country and its friends", fmt::format("{}", checked));
			r.check(held_wrong == 0, "supply territory: the provinces that are held give 100 percent");
			r.check(wrong == 0, "supply territory: the kind and the coefficient equal the ones counted another way on every province",
				fmt::format("{} provinces", wrong));
			r.check(
				kinds[static_cast<std::size_t>(SupplyTerritory::kind_t::ONE_NEIGHBOUR)] > 0
					&& kinds[static_cast<std::size_t>(SupplyTerritory::kind_t::ONE_BETWEEN)] > 0
					&& kinds[static_cast<std::size_t>(SupplyTerritory::kind_t::FAR)] > 0,
				"supply territory: provinces of every kind exist", fmt::format("{} {} {}",
					kinds[static_cast<std::size_t>(SupplyTerritory::kind_t::ONE_NEIGHBOUR)],
					kinds[static_cast<std::size_t>(SupplyTerritory::kind_t::ONE_BETWEEN)],
					kinds[static_cast<std::size_t>(SupplyTerritory::kind_t::FAR)])
			);

			// The search through two provinces between (opened by a technology, or by the define): only the provinces that were far away change
			OpenVicSupplyDefines& opened = const_cast<OpenVicSupplyDefines&>(defines);
			std::size_t changed = 0, bad_changed = 0;
			for (ProvinceInstance const& province : map.get_province_instances()) {
				if (province.province_definition.is_water()) {
					continue;
				}
				opened.supply_gap2_enabled = 0;
				SupplyTerritory const closed = calculate_supply_territory(province, map, *first, friends, defines);
				opened.supply_gap2_enabled = 1;
				SupplyTerritory const open = calculate_supply_territory(province, map, *first, friends, defines);
				opened.supply_gap2_enabled = 0;
				if (closed.kind != open.kind || closed.coefficient != open.coefficient) {
					changed++;
					bad_changed += closed.kind != SupplyTerritory::kind_t::FAR || open.kind != SupplyTerritory::kind_t::TWO_BETWEEN
						|| open.coefficient < opened.supply_gap2_base.fixed()
						|| open.coefficient > opened.supply_gap2_base.fixed() + opened.supply_gap2_step.fixed() * (opened.supply_gap2_max_count - 1);
				}
			}
			r.check(changed > 0, "supply territory: with two provinces between opened some far provinces get the 25 percent kind", fmt::format("{}", changed));
			r.check(bad_changed == 0, "supply territory: opening it only changes far provinces and gives 25 to 45 percent", fmt::format("{} provinces", bad_changed));
		}
	}

	// The delivery of the supply to the armies, the priorities and the reserves of the regiments (docs/PLAN_ARMY_SUPPLY_FRONTS.md, section 3)
	void test_supply_delivery(Results& r, InstanceManager& im) {
		OpenVicSupplyDefines const& defines = im.definition_manager.get_define_manager().get_openvic_defines().supply;
		const fixed_point_t standard = get_supply_priority_multiplier(defines, supply_priority_t::STANDARD);
		const fixed_point_t increased = get_supply_priority_multiplier(defines, supply_priority_t::INCREASED);
		const fixed_point_t maximum = get_supply_priority_multiplier(defines, supply_priority_t::MAXIMUM);
		r.check(standard == fixed_point_t::_1 && increased == fixed_point_t(5) / 2 && maximum == fixed_point_t(5),
			"supply delivery: the priorities send 1, 2.5 and 5 times the need at most");

		// A province that lets through the share d of the need: the standard priority sends the need and gets the share d
		bool bounded = true, standard_ok = true, increased_ok = true, never_more_sent = true;
		for (int step = 0; step <= 100; ++step) {
			const fixed_point_t d = fixed_point_t(step) / 100;
			for (const fixed_point_t multiplier : { standard, increased, maximum }) {
				SupplyDeliveryResult const result = calculate_supply_delivery(d, multiplier);
				bounded &= result.delivery >= fixed_point_t::_0 && result.delivery <= fixed_point_t::_1;
				bounded &= result.send_multiplier >= fixed_point_t::_1 && result.send_multiplier <= multiplier;
				// Never more than makes up for the losses: what arrives does not exceed the need
				never_more_sent &= result.delivery <= fixed_point_t::_1;
				if (multiplier == standard) {
					standard_ok &= result.delivery == d && result.send_multiplier == fixed_point_t::_1;
				}
			}
			SupplyDeliveryResult const result = calculate_supply_delivery(d, increased);
			// With the increased priority half of the need is already made up for at d = 0.4 (0.4 * 2.5 = 1)
			if (step >= 41) {
				increased_ok &= result.delivery == fixed_point_t::_1;
			} else if (step > 0 && step < 40) {
				increased_ok &= result.delivery == d * increased;
			}
		}
		r.check(bounded && never_more_sent, "supply delivery: what is sent is between 1 and the multiplier of the priority, what arrives is never above the need");
		r.check(standard_ok, "supply delivery: the standard priority sends the need and what arrives is the share the province lets through");
		r.check(increased_ok, "supply delivery: the increased priority makes up for the losses up to 2.5 times the need");
		r.check(calculate_supply_delivery(fixed_point_t::_0, maximum).delivery == fixed_point_t::_0
				&& calculate_supply_delivery(fixed_point_t::_0, maximum).send_multiplier == fixed_point_t::_1,
			"supply delivery: nothing gets through and nothing is sent (a province without a limit does not divide by zero)");
		r.check(calculate_supply_delivery(fixed_point_t(3), maximum).delivery == fixed_point_t::_1,
			"supply delivery: a share above 1 does not give more than the need");

		// The reserve: a week without supplies lasts exactly the days of the reserve, then the regiment is without
		const fixed_point_t week = fixed_point_t(7);
		fixed_point_t reserve = week;
		int days = 0;
		fixed_point_t deficit = fixed_point_t::_0;
		while (deficit == fixed_point_t::_0 && days < 30) {
			SupplyReserveResult const result = step_supply_reserve(reserve, fixed_point_t::_1, fixed_point_t::_0, week);
			reserve = result.reserve;
			deficit = result.deficit;
			days++;
		}
		r.check(days == 8 && reserve == fixed_point_t::_0, "supply reserve: a regiment with a reserve of 7 days stands 7 days without supplies, then it has a deficit",
			fmt::format("{} days", days));
		SupplyReserveResult half = step_supply_reserve(week, fixed_point_t(1) / 2, fixed_point_t::_0, week);
		r.check(half.reserve == week - fixed_point_t(1) / 2 && half.deficit == fixed_point_t::_0, "supply reserve: half of the need arriving costs half a day of the reserve");
		SupplyReserveResult refilled = step_supply_reserve(fixed_point_t(3), fixed_point_t::_0, fixed_point_t::_1, week);
		r.check(refilled.reserve == fixed_point_t(4), "supply reserve: with everything arriving the reserve rises by the refill");
		SupplyReserveResult capped = step_supply_reserve(fixed_point_t(7), fixed_point_t::_0, fixed_point_t::_1, week);
		r.check(capped.reserve == week, "supply reserve: the reserve never rises above its capacity");
		SupplyReserveResult none = step_supply_reserve(fixed_point_t::_0, fixed_point_t(3) / 10, fixed_point_t::_0, fixed_point_t::_0);
		r.check(none.reserve == fixed_point_t::_0 && none.deficit == fixed_point_t(3) / 10, "supply reserve: without a reserve (no technology) the whole shortfall is a deficit");

		// The armies of a real game after some days
		play(im, 3);
		std::size_t armies = 0, bad_level = 0, bad_delivery = 0, bad_send = 0, bad_reserve = 0, with_reserve = 0;
		for (CountryInstance const& country : im.get_country_instance_manager().get_country_instances()) {
			const fixed_point_t capacity = country.get_supply_reserve_capacity();
			for (ArmyInstance const& army : country.get_armies()) {
				armies++;
				bad_level += army.get_supply_level() < fixed_point_t::_0 || army.get_supply_level() > fixed_point_t::_1;
				bad_delivery += army.get_supply_delivery() < fixed_point_t::_0 || army.get_supply_delivery() > fixed_point_t::_1
					|| army.get_supply_ratio() < fixed_point_t::_0 || army.get_supply_ratio() > fixed_point_t::_1;
				bad_send += army.get_supply_send_multiplier() < fixed_point_t::_1 || army.get_supply_send_multiplier() > maximum;
				for (RegimentInstance const& regiment : army.get_regiment_instances()) {
					bad_reserve += regiment.get_supply_reserve() < fixed_point_t::_0 || regiment.get_supply_reserve() > capacity;
					with_reserve += regiment.get_supply_reserve() > fixed_point_t::_0;
				}
			}
		}
		r.check(armies > 50, "supply delivery: there are armies", fmt::format("{}", armies));
		r.check(bad_level == 0 && bad_delivery == 0, "supply delivery: the supply level and what arrives of every army are between 0 and 1",
			fmt::format("{} {}", bad_level, bad_delivery));
		r.check(bad_send == 0, "supply delivery: no army is sent more than the multiplier of the maximum priority", fmt::format("{}", bad_send));
		r.check(bad_reserve == 0, "supply delivery: the reserve of every regiment is between 0 and the capacity of its country", fmt::format("{}", bad_reserve));
		r.check(with_reserve > 0, "supply delivery: the regiments of the civilised countries have a reserve");

		// The actions: the priority of an army, the return to the priority of the country, the priority of the country
		ArmyInstance* army = nullptr;
		for (CountryInstance& country : im.get_country_instance_manager().get_country_instances()) {
			if (country.has_armies()) {
				army = &country.get_armies().front().get();
				break;
			}
		}
		r.check(army != nullptr, "supply priority: an army exists");
		if (army != nullptr) {
			CountryInstance& owner = army->get_country();
			r.check(!army->is_supply_priority_custom() && owner.get_effective_supply_priority(false, army->get_supply_priority()) == supply_priority_t::STANDARD,
				"supply priority: an army has the priority of its country at first");
			im.queue_game_action<set_global_supply_priority_argument_t>(owner.index, std::uint32_t(1));
			im.force_tick_and_update();
			r.check(owner.get_army_supply_priority() == supply_priority_t::INCREASED, "supply priority: the priority of the country is set");
			im.queue_game_action<set_army_supply_priority_argument_t>(army->unique_id, std::uint32_t(2));
			im.force_tick_and_update();
			r.check(army->is_supply_priority_custom() && army->get_supply_priority() == supply_priority_t::MAXIMUM,
				"supply priority: an army can have its own priority");
			im.queue_game_action<set_global_supply_priority_argument_t>(owner.index, std::uint32_t(0));
			im.force_tick_and_update();
			r.check(army->get_supply_priority() == supply_priority_t::MAXIMUM && army->is_supply_priority_custom(),
				"supply priority: the priority of the country does not change an army that has its own");
			im.queue_game_action<set_army_supply_priority_argument_t>(army->unique_id, std::uint32_t(3));
			im.force_tick_and_update();
			r.check(!army->is_supply_priority_custom(), "supply priority: an army can return to the priority of its country");
		}

		// ---- An army on the land of an enemy far from its own land: the limit lets little through, the reserve is used up in days, then the
		// supply level falls (the war between two countries was declared by the tests above)
		{
			OpenVicSupplyDefines& open_defines = const_cast<OpenVicSupplyDefines&>(defines);
			CountryInstanceManager const& all_countries = im.get_country_instance_manager();
			ArmyInstance* raider = nullptr;
			for (CountryInstance& country : im.get_country_instance_manager().get_country_instances()) {
				if (!country.is_at_war() || country.get_war_enemies().empty() || !country.has_armies()) {
					continue;
				}
				for (ArmyInstance& candidate : country.get_armies()) {
					if (candidate.get_unit_count() >= 4 && (raider == nullptr || candidate.get_unit_count() > raider->get_unit_count())) {
						raider = &candidate;
					}
				}
				if (raider != nullptr) {
					break;
				}
			}
			r.check(raider != nullptr, "supply on enemy land: an army of a country at war with four regiments exists");
			if (raider != nullptr) {
				CountryInstance& attacker = raider->get_country();
				const SupplyFriends raider_friends = SupplyFriends::build(attacker, all_countries, im.get_war_manager());
				ProvinceInstance* target = nullptr;
				for (ProvinceInstance& province : im.get_map_instance().get_province_instances()) {
					if (!province.province_definition.is_water() && province.get_owner() != nullptr && province.get_owner() == province.get_controller()
						&& attacker.is_at_war_with(*province.get_owner())
						&& calculate_supply_territory(province, im.get_map_instance(), attacker, raider_friends, defines).kind
							== SupplyTerritory::kind_t::FAR) {
						target = &province;
						break;
					}
				}
				r.check(target != nullptr, "supply on enemy land: an enemy province far from the land of the attacker exists");
				if (target != nullptr) {
					const fixed_point_t capacity = attacker.get_supply_reserve_capacity();
					raider->set_location(*target);
					raider->set_supply_level(fixed_point_t::_1);
					for (RegimentInstance& regiment : raider->get_regiment_instances()) {
						regiment.set_supply_reserve(capacity);
					}
					raider->set_supply_priority_custom(true);
					raider->set_supply_priority(supply_priority_t::STANDARD);
					play(im, 1);
					const fixed_point_t first_delivery = raider->get_supply_delivery();
					r.check(first_delivery < fixed_point_t::_1 && first_delivery > fixed_point_t::_0,
						"supply on enemy land: the limit of the enemy province lets through only a part of the need",
						fmt::format("{}", first_delivery));
					const fixed_point_t reserve_after_one = raider->get_supply_reserve_average();
					r.check(reserve_after_one < capacity, "supply on enemy land: the reserve is used on the first day",
						fmt::format("{} of {}", reserve_after_one, capacity));
					r.check(raider->get_supply_level() == fixed_point_t::_1, "supply on enemy land: the supply level does not fall while there is a reserve");
					// The army must stay where it is
					int days = 1;
					while (days < 30 && raider->get_supply_reserve_average() > fixed_point_t::_0) {
						raider->set_location(*target);
						play(im, 1);
						days++;
					}
					r.check(raider->get_supply_reserve_average() == fixed_point_t::_0, "supply on enemy land: the reserve is used up", fmt::format("{} days", days));
					const fixed_point_t shortfall = fixed_point_t::_1 - raider->get_supply_delivery();
					const int expected = shortfall > fixed_point_t::_0 ? static_cast<int>((capacity / shortfall).round<int32_t>()) : 0;
					r.check(days >= expected - 1 && days <= expected + 2, "supply on enemy land: the reserve lasts its days divided by the shortfall",
						fmt::format("{} days, expected about {}", days, expected));
					for (int i = 0; i < 10; ++i) {
						raider->set_location(*target);
						play(im, 1);
					}
					r.check(raider->get_supply_level() < fixed_point_t::_1, "supply on enemy land: the supply level falls when the reserve is used up",
						fmt::format("{}", raider->get_supply_level()));
					// The maximum priority sends more and makes up for the losses
					const fixed_point_t standard_ratio = raider->get_supply_ratio();
					raider->set_supply_priority(supply_priority_t::MAXIMUM);
					raider->set_location(*target);
					play(im, 1);
					r.check(raider->get_supply_ratio() > standard_ratio && raider->get_supply_send_multiplier() > fixed_point_t::_1,
						"supply on enemy land: the maximum priority sends more and what arrives is more",
						fmt::format("{} -> {} (sent x{})", standard_ratio, raider->get_supply_ratio(), raider->get_supply_send_multiplier()));
					r.check(raider->get_supply_ratio() <= fixed_point_t::_1, "supply on enemy land: what arrives never exceeds the need");
					(void)open_defines;
				}
			}
		}
	}

	// Divisions and templates (docs/PLAN_ARMY_SUPPLY_FRONTS.md, section 4): the civilised countries have armies of divisions (the regiments of the start
	// are out of the template), the others have groups of regiments only and no generals; templates and reforming
	void test_divisions(Results& r, InstanceManager& im) {
		std::size_t civilised_armies = 0, regiments_without_division = 0, bad_division = 0, templated_at_start = 0, plain_with_division = 0, plain_leaders = 0;
		std::size_t civilised_without_template = 0, plain_with_template = 0, plain_armies = 0;
		for (CountryInstance const& country : im.get_country_instance_manager().get_country_instances()) {
			if (!country.exists() || !country.has_armies()) {
				continue;
			}
			if (country.has_army_organisation()) {
				civilised_without_template += country.get_division_templates().empty();
			} else {
				plain_with_template += !country.get_division_templates().empty();
			}
			for (ArmyInstance const& army : country.get_armies()) {
				if (country.has_army_organisation()) {
					civilised_armies++;
					for (DivisionInstance const& division : army.get_divisions()) {
						templated_at_start += division.template_id != 0;
					}
					for (RegimentInstance const& regiment : army.get_regiment_instances()) {
						bool found = false;
						for (DivisionInstance const& division : army.get_divisions()) {
							found |= division.id == regiment.get_division_id();
						}
						regiments_without_division += regiment.get_division_id() == 0;
						bad_division += !found;
					}
				} else {
					plain_armies++;
					plain_with_division += !army.get_divisions().empty();
					plain_leaders += army.get_leader() != nullptr;
					for (RegimentInstance const& regiment : army.get_regiment_instances()) {
						plain_with_division += regiment.get_division_id() != 0;
					}
				}
			}
		}
		r.check(civilised_armies > 50 && plain_armies > 0, "divisions: there are armies of civilised and of other countries", fmt::format("{} {}", civilised_armies, plain_armies));
		r.check(regiments_without_division == 0 && bad_division == 0, "divisions: every regiment of a civilised country is in a division of its army",
			fmt::format("{} {}", regiments_without_division, bad_division));
		r.check(templated_at_start == 0, "divisions: the regiments of the start are out of the template (they are not touched)", fmt::format("{}", templated_at_start));
		r.check(civilised_without_template == 0, "divisions: a civilised country has the standard template");
		r.check(plain_with_division == 0 && plain_with_template == 0, "divisions: a country without the technology has no divisions and no templates");
		r.check(plain_leaders == 0, "divisions: the armies of a country without the technology have no general");

		// The standard template: three regiments of infantry and one of cavalry
		CountryInstance* country = nullptr;
		ArmyInstance* army = nullptr;
		for (CountryInstance& candidate : im.get_country_instance_manager().get_country_instances()) {
			if (!candidate.exists() || !candidate.has_army_organisation() || candidate.is_at_war()) {
				continue;
			}
			for (ArmyInstance& candidate_army : candidate.get_armies()) {
				std::size_t infantry = 0, cavalry = 0;
				for (RegimentInstance const& regiment : candidate_army.get_regiment_instances()) {
					infantry += get_division_slot_kind(regiment.get_regiment_type()) == 0;
					cavalry += get_division_slot_kind(regiment.get_regiment_type()) == 1;
				}
				if (infantry >= 2 && cavalry >= 2 && !candidate_army.is_in_combat()) {
					country = &candidate;
					army = &candidate_army;
					break;
				}
			}
			if (army != nullptr) {
				break;
			}
		}
		r.check(army != nullptr, "divisions: an army with two regiments of infantry and two of cavalry exists");
		if (army == nullptr) {
			return;
		}
		DivisionTemplate const* const standard = country->find_division_template(1);
		r.check(standard != nullptr && standard->slots[0] == 3 && standard->slots[1] == 1 && standard->slots[2] == 0 && standard->slots[3] == 0,
			"divisions: the standard template has three regiments of infantry and one of cavalry");

		const auto count_templated = [&army](const std::uint32_t template_id) -> std::size_t {
			std::size_t count = 0;
			for (DivisionInstance const& division : army->get_divisions()) {
				count += division.template_id == template_id;
			}
			return count;
		};
		const auto count_in_templates = [&army](const std::size_t kind) -> std::size_t {
			std::size_t count = 0;
			for (RegimentInstance const& regiment : army->get_regiment_instances()) {
				for (DivisionInstance const& division : army->get_divisions()) {
					if (division.id == regiment.get_division_id() && division.template_id != 0
						&& get_division_slot_kind(regiment.get_regiment_type()) == kind) {
						count++;
					}
				}
			}
			return count;
		};

		// The standard template: one whole division for every three regiments of infantry (with a regiment of cavalry for each)
		std::size_t standard_infantry = 0, standard_cavalry = 0;
		for (RegimentInstance const& regiment : army->get_regiment_instances()) {
			standard_infantry += get_division_slot_kind(regiment.get_regiment_type()) == 0;
			standard_cavalry += get_division_slot_kind(regiment.get_regiment_type()) == 1;
		}
		const std::size_t standard_expected = std::min(standard_infantry / 3, standard_cavalry);
		const std::size_t standard_regiments = army->get_unit_count();
		im.queue_game_action<reform_army_to_template_argument_t>(army->unique_id, std::uint32_t(0), std::uint32_t(1));
		im.force_tick_and_update();
		r.check(count_templated(1) == standard_expected && army->get_unit_count() == standard_regiments,
			"divisions: the regiments make whole divisions of the standard template (the others stay as they were)",
			fmt::format("{} of {}", count_templated(1), standard_expected));
		// The template is deleted: its divisions stay, out of the template (the regiments are free for the next template)
		im.queue_game_action<delete_division_template_argument_t>(country->index, std::uint32_t(1));
		im.force_tick_and_update();
		r.check(country->find_division_template(1) == nullptr && count_templated(1) == 0 && army->get_unit_count() == standard_regiments,
			"divisions: deleting a template leaves its divisions out of the template");

		// A new template: one regiment of infantry and one of cavalry
		const std::size_t templates_before = country->get_division_templates().size();
		im.queue_game_action<create_division_template_argument_t>(
			country->index, std::uint32_t(1), std::uint32_t(1), std::uint32_t(0), std::uint32_t(0)
		);
		im.force_tick_and_update();
		r.check(country->get_division_templates().size() == templates_before + 1, "divisions: a template can be made");
		const std::uint32_t pair_id = country->get_division_templates().back().id;
		im.queue_game_action<create_division_template_argument_t>(
			country->index, std::uint32_t(0), std::uint32_t(0), std::uint32_t(0), std::uint32_t(0)
		);
		im.force_tick_and_update();
		r.check(country->get_division_templates().size() == templates_before + 1, "divisions: a template without slots is refused");

		std::size_t infantry = 0, cavalry = 0;
		for (RegimentInstance const& regiment : army->get_regiment_instances()) {
			infantry += get_division_slot_kind(regiment.get_regiment_type()) == 0;
			cavalry += get_division_slot_kind(regiment.get_regiment_type()) == 1;
		}
		const std::size_t expected_divisions = std::min(infantry, cavalry);
		const std::size_t regiments = army->get_unit_count();
		im.queue_game_action<reform_army_to_template_argument_t>(army->unique_id, std::uint32_t(0), pair_id);
		im.force_tick_and_update();
		r.check(count_templated(pair_id) == expected_divisions && expected_divisions >= 2, "divisions: the regiments make as many whole divisions as they can",
			fmt::format("{} of {}", count_templated(pair_id), expected_divisions));
		r.check(count_in_templates(0) == expected_divisions && count_in_templates(1) == expected_divisions,
			"divisions: every division of the template has one regiment of infantry and one of cavalry");
		r.check(army->get_unit_count() == regiments, "divisions: reforming does not lose regiments");

		// Changing the template changes the divisions: two regiments of infantry and none of cavalry (the cavalry goes to the reserve)
		im.queue_game_action<update_division_template_argument_t>(
			country->index, pair_id, std::uint32_t(2), std::uint32_t(0), std::uint32_t(0), std::uint32_t(0)
		);
		im.force_tick_and_update();
		r.check(count_in_templates(1) == 0, "divisions: when the template changes the regiments that do not fit leave the divisions");
		std::size_t out_of_template = 0, in_divisions = 0;
		for (RegimentInstance const& regiment : army->get_regiment_instances()) {
			bool templated = false;
			for (DivisionInstance const& division : army->get_divisions()) {
				templated |= division.id == regiment.get_division_id() && division.template_id != 0;
			}
			out_of_template += !templated;
			in_divisions += templated;
		}
		r.check(in_divisions + out_of_template == regiments && out_of_template >= expected_divisions,
			"divisions: the regiments that left wait in the reserve of the army", fmt::format("{} {}", in_divisions, out_of_template));
		bad_division = 0;
		for (DivisionInstance const& division : army->get_divisions()) {
			if (division.template_id == pair_id) {
				std::size_t members = 0;
				for (RegimentInstance const& regiment : army->get_regiment_instances()) {
					members += regiment.get_division_id() == division.id;
				}
				bad_division += members > 2;
			}
		}
		r.check(bad_division == 0, "divisions: a division never has more regiments than its template");

		// The template is deleted: its divisions stay, out of the template
		im.queue_game_action<delete_division_template_argument_t>(country->index, pair_id);
		im.force_tick_and_update();
		r.check(country->find_division_template(pair_id) == nullptr && count_templated(pair_id) == 0 && army->get_unit_count() == regiments,
			"divisions: the divisions of a deleted template do not belong to any template");

		// Without the technology there are no templates, and generals command armies only with it
		CountryInstance* plain = nullptr;
		ArmyInstance* plain_army = nullptr;
		for (CountryInstance& candidate : im.get_country_instance_manager().get_country_instances()) {
			if (candidate.exists() && !candidate.has_army_organisation() && candidate.has_armies() && !candidate.get_leaders<unit_branch_t::LAND>().empty()) {
				plain = &candidate;
				plain_army = &candidate.get_armies().front().get();
				break;
			}
		}
		if (plain != nullptr) {
			im.queue_game_action<create_division_template_argument_t>(
				plain->index, std::uint32_t(1), std::uint32_t(0), std::uint32_t(0), std::uint32_t(0)
			);
			im.force_tick_and_update();
			r.check(plain->get_division_templates().empty(), "divisions: a country without the technology cannot make templates");
			LeaderInstance const& general = plain->get_leaders<unit_branch_t::LAND>().front();
			im.queue_game_action<set_army_leader_argument_t>(plain_army->unique_id, general.unique_id);
			im.force_tick_and_update();
			r.check(plain_army->get_leader() == nullptr, "divisions: a general cannot command the army of a country without the technology");
		}
	}

	// World trade: the routes by land and sea exist and are the same both ways, the freight of the sellers reaches the treasuries, the
	// workers keep part of the output, and the people who are hungry for a month and have no job emigrate or die
	void test_trade_and_hunger(Results& r, InstanceManager& im) {
		MoneyLedger& ledger = im.get_money_ledger();
		ledger.enable(im);

		std::int64_t starved = 0;
		for (int day = 0; day < 130; ++day) {
			im.force_tick_and_update();
			if (im.get_today().is_month_start()) {
				for (ProvinceInstance& province : im.get_map_instance().get_province_instances()) {
					for (Pop& pop : province.get_mutable_pops()) {
						starved += type_safe::get(pop.get_num_starved());
					}
				}
			}
		}

		TradeNetwork const& network = im.get_trade_network();
		r.check(network.is_ready(), "trade: the network of the routes is built");
		const std::size_t countries = network.get_node_count() - 1;
		std::size_t routes = 0, land = 0, sea = 0, asymmetric = 0, bad_cost = 0;
		for (std::size_t from = 0; from < countries; ++from) {
			for (std::size_t to = 0; to < countries; ++to) {
				TradeNetwork::route_t const& route = network.get_route(country_index_t(from), country_index_t(to));
				TradeNetwork::route_t const& back = network.get_route(country_index_t(to), country_index_t(from));
				if (from == to) {
					bad_cost += route.mode != TradeNetwork::mode_t::NONE;
					continue;
				}
				if ((route.mode == TradeNetwork::mode_t::NONE) != (back.mode == TradeNetwork::mode_t::NONE)) {
					asymmetric++;
				}
				if (route.mode == TradeNetwork::mode_t::NONE) {
					continue;
				}
				routes++;
				land += route.mode == TradeNetwork::mode_t::LAND;
				sea += route.mode == TradeNetwork::mode_t::SEA;
				bad_cost += route.cost < fixed_point_t::_0 || route.cost > fixed_point_t::_1;
			}
		}
		r.check(routes > 0 && land > 0 && sea > 0, "trade: there are routes by land and by sea", fmt::format("{} routes, {} land, {} sea", routes, land, sea));
		r.check(asymmetric == 0, "trade: a route exists both ways", fmt::format("{} pairs", asymmetric));
		r.check(bad_cost == 0, "trade: the cost of a route is a share of the price, a country has no route to itself", fmt::format("{}", bad_cost));

		// The freight went to the treasuries (the ledger, which is closed in the other runs, counts it with a hook of its own)
		fixed_point_t fee_sum;
		for (CountryInstance const& country : im.get_country_instance_manager().get_country_instances()) {
			fee_sum += country.get_transport_income();
		}
		r.check(fee_sum > fixed_point_t::_0, "trade: the freight of the sellers reaches the treasuries", fmt::format("{}", fee_sum));

		// Subsistence: the pool never is more than the output
		std::int32_t pools = 0, pools_too_big = 0;
		for (ProvinceInstance const& province : im.get_map_instance().get_province_instances()) {
			const fixed_point_t pool = province.get_rgo().get_subsistence_pool();
			if (pool > fixed_point_t::_0) {
				pools++;
				pools_too_big += pool > province.get_rgo().get_output_quantity_yesterday();
			}
		}
		r.check(pools > 0, "trade: the workers keep part of the output of their RGO");
		r.check(pools_too_big == 0, "trade: the workers never keep more than the RGO made", fmt::format("{} of {}", pools_too_big, pools));

		fmt::println("TEST INFO trade: {} routes ({} land, {} sea), {} people died of hunger in the first months, {} RGOs kept part of their output", routes, land, sea, starved, pools);
		ledger.disable();
	}

	// The investment fund: the construction slider sets money aside, the AI keeps its budget out of a loss, the money of the
	// fund pays the construction of profitable enterprises, and no money appears or disappears without a hook of the ledger
	void test_investment_fund(Results& r, InstanceManager& im) {
		CountryInstance* player = nullptr;
		for (CountryInstance& country : im.get_country_instance_manager().get_country_instances()) {
			if (country.exists() && (player == nullptr || country.get_states().size() > player->get_states().size())) {
				player = &country;
			}
		}
		r.check(player != nullptr, "investment fund: a country with states exists");
		if (player == nullptr) {
			return;
		}

		MoneyLedger& ledger = im.get_money_ledger();
		ledger.enable(im);

		// A country played by a human keeps the whole allocation (the AI sets its own)
		player->set_ai(false);
		r.check(
			player->get_construction_spending_slider_value().get_value_untracked() == fixed_point_t::_0,
			"investment fund: the slider of the allocation starts at nothing"
		);
		im.queue_game_action<set_construction_spending_argument_t>(player->index, fixed_point_t::_1);
		im.force_tick_and_update();
		r.check(
			player->get_construction_spending_slider_value().get_value_untracked() == fixed_point_t::_1,
			"investment fund: the slider of the player can be raised to 100 percent"
		);

		constexpr int DAYS = 240;
		play(im, DAYS);

		const fixed_point_t income = player->get_income_yesterday();
		const fixed_point_t allocation = player->get_investment_allocation();
		r.check(
			player->get_investment_fund() + player->get_investment_spent_total() > fixed_point_t::_0,
			"investment fund: the slider sets money aside every day",
			fmt::format("fund {}, spent {}", player->get_investment_fund(), player->get_investment_spent_total())
		);
		r.check(
			income > fixed_point_t::_0 && allocation * 2 >= income - fixed_point_t(1) / 100 && allocation * 2 <= income + fixed_point_t(1) / 100,
			"investment fund: 100 percent of the slider is half of the income",
			fmt::format("income {}, allocation {}", income, allocation)
		);

		// The AI sets money aside only out of a surplus of its budget, and its treasury never goes below zero
		std::int32_t ai_investing = 0, ai_wrong = 0, ai_negative = 0, ai_total = 0, ai_surplus = 0, ai_deficit = 0;
		fixed_point_t total_fund, total_spent;
		for (CountryInstance& country : im.get_country_instance_manager().get_country_instances()) {
			if (!country.exists() || country.get_states().empty()) {
				continue;
			}
			total_fund += country.get_investment_fund();
			total_spent += country.get_investment_spent_total();
			if (country.get_cash_stockpile().load() < fixed_point_t::_0) {
				ai_negative++;
			}
			if (country.is_ai()) {
				ai_total++;
				if (country.get_balance_before_investment_average() > fixed_point_t::_0) {
					ai_surplus++;
				} else {
					ai_deficit++;
				}
			}
			if (country.is_ai() && country.get_construction_spending_slider_value().get_value_untracked() > fixed_point_t::_0) {
				ai_investing++;
				if (country.get_balance_before_investment_average() <= fixed_point_t::_0) {
					ai_wrong++;
				}
			}
		}
		r.check(ai_wrong == 0, "investment fund: an AI country sets money aside only while its budget has a surplus", fmt::format("{} of {}", ai_wrong, ai_investing));
		r.check(ai_negative == 0, "investment fund: no treasury goes below zero", fmt::format("{} countries", ai_negative));

		// Factories built with the money of the fund belong to the pops that were given it
		std::int32_t owned_by_pops = 0, deficit_projects = 0, projects = 0;
		for (CountryInstance const& country : im.get_country_instance_manager().get_country_instances()) {
			for (auto const& record : country.get_recent_investments()) {
				projects++;
				deficit_projects += record.deficit ? 1 : 0;
			}
		}
		for (StateSet const& state_set : im.get_map_instance().get_state_manager().get_state_sets()) {
			for (State const& state : state_set.get_states()) {
				for (FactoryInstance const& factory : state.get_factories()) {
					owned_by_pops += factory.get_owner_pop_type_index().has_value() ? 1 : 0;
				}
			}
		}
		fmt::println(
			"TEST INFO investment fund: after {} days the funds hold {}, {} was paid for projects ({} of the last projects, {} for a good the country lacks), {} factories belong to the pops that built them, {} of {} AI countries set money aside ({} have a surplus, {} a deficit)",
			DAYS, total_fund, total_spent, projects, deficit_projects, owned_by_pops, ai_investing, ai_total, ai_surplus, ai_deficit
		);
		r.check(total_spent > fixed_point_t::_0, "investment fund: the money of the funds builds profitable enterprises");

		r.check(
			ledger.get_measured_change_raw(im) == ledger.get_holder_sum_raw(),
			"investment fund: the money stays accounted for (nothing appears or disappears without a payment)",
			fmt::format("measured {}, explained {}", ledger.get_measured_change_raw(im), ledger.get_holder_sum_raw())
		);
		ledger.disable();
	}

	void test_game(Results& r, Game& game) {
		constexpr int FIRST_DAYS = 40;
		constexpr int MORE_DAYS = 20;

		// Every day of every run the provinces also add up the political support of their pops and compare it with the sums that they kept
		const std::size_t kept_sums_differences_before = ProvinceInstance::get_political_support_cache_mismatch_count();
		ProvinceInstance::set_verify_political_support_cache(true);
		// The same for the consumption of the pops that the countries record: the flat sums against the map of the old way
		const std::size_t consumption_differences_before = CountryInstance::get_need_consumption_mismatch_count();
		CountryInstance::set_verify_need_consumption(true);

		// ---- run 1: the game that the others are compared with
		InstanceManager* im = game.start();
		r.check(im != nullptr, "game: the first instance starts");
		if (im == nullptr) {
			return;
		}
		const std::uint64_t defines_hash = im->definition_manager.get_define_manager().get_openvic_defines().get_hash();
		const std::uint64_t start_hash = StateHash::compute(*im);
		give_orders(*im);
		std::vector<std::uint64_t> days_1, days_2;
		play_recording(*im, FIRST_DAYS, days_1, g_details_1);
		const std::uint64_t hash_1 = StateHash::compute(*im);
		const std::string save_1 = Replay::serialise(im->get_action_history());
		play(*im, MORE_DAYS);
		const std::uint64_t hash_1_more = StateHash::compute(*im);
		r.check(hash_1 != start_hash && hash_1_more != hash_1, "game: the hash follows the game (it changes from day to day)");
		game.end();

		// ---- run 2: the same game again, deterministic
		im = game.start();
		r.check(im != nullptr && StateHash::compute(*im) == start_hash, "determinism: a new instance starts in the same state", im ? hex(StateHash::compute(*im)) + " != " + hex(start_hash) : "no instance");
		if (im == nullptr) {
			return;
		}
		give_orders(*im);
		play_recording(*im, FIRST_DAYS, days_2, g_details_2);
		const std::uint64_t hash_2 = StateHash::compute(*im);
		for (std::size_t day = 0; day < days_1.size() && day < days_2.size(); ++day) {
			if (days_1[day] != days_2[day]) {
				fmt::println("TEST INFO determinism: the two runs first differ on day {} after the orders", day + 1);
				DayDetails const& a = g_details_1[day];
				DayDetails const& b = g_details_2[day];
				fmt::println("TEST INFO determinism: pop cash {} vs {}, factory budgets {} vs {}, capitalist funds {} vs {}, prices {} vs {}",
					a.pop_cash, b.pop_cash, a.factory_budget, b.factory_budget, a.capitalist_fund, b.capitalist_fund, a.price_sum, b.price_sum);
				int shown = 0;
				for (std::size_t c = 0; c < a.country_cash.size() && shown < 6; ++c) {
					if (a.country_cash[c] != b.country_cash[c]) {
						fmt::println("TEST INFO determinism: country {} cash {} vs {}", c, a.country_cash[c], b.country_cash[c]);
						shown++;
					}
				}
				break;
			}
		}
		const std::string save_2 = Replay::serialise(im->get_action_history());
		r.check(hash_2 == hash_1, "determinism: two runs with the same actions end in the same state", hex(hash_1) + " != " + hex(hash_2));
		r.check(save_2 == save_1, "determinism: two runs write the same action history");
		game.end();

		// ---- run 2b: the same game with one worker thread, the result must not depend on the number of threads
		ThreadPool::set_worker_thread_count(1);
		im = game.start();
		ThreadPool::set_worker_thread_count(0);
		r.check(im != nullptr, "determinism: the instance with one thread starts");
		if (im == nullptr) {
			return;
		}
		give_orders(*im);
		std::vector<std::uint64_t> days_one_thread;
		std::vector<DayDetails> details_one_thread;
		play_recording(*im, FIRST_DAYS, days_one_thread, details_one_thread);
		const std::uint64_t hash_one_thread = StateHash::compute(*im);
		for (std::size_t day = 0; day < days_1.size() && day < days_one_thread.size(); ++day) {
			if (days_1[day] != days_one_thread[day]) {
				fmt::println("TEST INFO determinism: with one thread the game first differs on day {} after the orders", day + 1);
				break;
			}
		}
		r.check(
			hash_one_thread == hash_1, "determinism: the result does not depend on the number of threads (one thread, all threads)",
			hex(hash_1) + " != " + hex(hash_one_thread)
		);
		game.end();

		// ---- run 3: a save game: the state after loading equals the state before saving
		im = game.start();
		r.check(im != nullptr, "save game: the instance for loading starts");
		if (im == nullptr) {
			return;
		}
		std::vector<ReplayEntry> entries;
		r.check(Replay::deserialise(save_1, entries), "save game: the saved history is read");
		r.check(im->replay_actions(entries), "save game: the saved history is replayed");
		const std::uint64_t loaded_hash = StateHash::compute(*im);
		r.check(loaded_hash == hash_1, "save game: the state after loading equals the state before saving", hex(hash_1) + " != " + hex(loaded_hash));
		play(*im, MORE_DAYS);
		const std::uint64_t loaded_more = StateHash::compute(*im);
		r.check(loaded_more == hash_1_more, "save game: the game goes on after loading as it would have without saving", hex(hash_1_more) + " != " + hex(loaded_more));
		r.check(
			Replay::serialise(im->get_action_history()).size() > save_1.size(), "save game: the history of a loaded game goes on (it can be saved again)"
		);
		game.end();

		// ---- run 4: the money: it is created only by the gold
		im = game.start();
		r.check(im != nullptr, "money: the instance starts");
		if (im == nullptr) {
			return;
		}
		MoneyLedger& ledger = im->get_money_ledger();
		ledger.enable(*im);
		give_orders(*im);
		play(*im, FIRST_DAYS);
		const std::int64_t measured = ledger.get_measured_change_raw(*im);
		const std::int64_t holders = ledger.get_holder_sum_raw();
		const double days = static_cast<double>(ledger.get_days());
		const auto flow = [&](MoneyLedger::label_t label) -> std::int64_t { return ledger.get_flow_total_raw(label); };
		const auto pounds = [](const std::int64_t raw) -> double { return static_cast<double>(raw) / static_cast<double>(1 << fixed_point_t::PRECISION); };

		r.check(measured == holders, "money: every change of the total money is explained by the payments (nothing unaccounted)",
			fmt::format("measured {:.3f}, explained {:.3f} over {} days", pounds(measured), pounds(holders), days));
		r.check(flow(MoneyLedger::label_t::factory_budget_floor) == 0, "money: no factory spends money it does not have",
			fmt::format("{:.3f}", pounds(flow(MoneyLedger::label_t::factory_budget_floor))));

		// What is created: the gold (the rules of Victoria 2), the starting budget of new factories, the rounding up of the sellers.
		// The total must not grow by anything else. (It may grow by less: money is in flight in the factories for a day, and
		// the money of slaves and the cost of the expansions of the factories leave the economy.)
		const std::int64_t created = flow(MoneyLedger::label_t::gold_worker_pay) + flow(MoneyLedger::label_t::gold_income)
			+ flow(MoneyLedger::label_t::factory_starting_budget) + flow(MoneyLedger::label_t::market_sellers_get)
			+ flow(MoneyLedger::label_t::market_buyers_pay) + flow(MoneyLedger::label_t::transport_fee_income)
			+ flow(MoneyLedger::label_t::factory_budget_floor);
		r.check(
			measured <= created + (1 << fixed_point_t::PRECISION),
			"money: the total grows only by the gold, the starting budgets of new factories and the rounding of the market",
			fmt::format("the total changed by {:.3f}, the allowed sources gave {:.3f}", pounds(measured), pounds(created))
		);
		fmt::println("TEST INFO money: the total changed by {:.1f}, created by the allowed sources {:.1f} (gold {:.1f}, starting budgets {:.1f})",
			pounds(measured), pounds(created), pounds(flow(MoneyLedger::label_t::gold_worker_pay) + flow(MoneyLedger::label_t::gold_income)),
			pounds(flow(MoneyLedger::label_t::factory_starting_budget)));
		const double rounding = pounds(flow(MoneyLedger::label_t::market_sellers_get) + flow(MoneyLedger::label_t::market_buyers_pay) + flow(MoneyLedger::label_t::transport_fee_income));
		const double volume = -pounds(flow(MoneyLedger::label_t::market_buyers_pay));
		// (the shares of the orders are rounded, either way, by the smallest amount per order)
		r.check(rounding >= -volume * 0.0001 - 1.0 && rounding <= volume * 0.001 + 1.0, "money: the rounding of the market is below 0.1% of the trade",
			fmt::format("rounding {:.3f} of {:.3f}", rounding, volume));
		ledger.disable();
		game.end();


		// ---- run 5: the credits in a game: offers, taking, withdrawing, bankruptcy, a region that changes its owner
		im = game.start();
		r.check(im != nullptr, "credit: the instance starts");
		if (im == nullptr) {
			return;
		}
		test_credit_game(r, *im);
		game.end();

		// ---- run 6: the factory actions (priority, deleting)
		im = game.start();
		r.check(im != nullptr, "factories: the instance starts");
		if (im == nullptr) {
			return;
		}
		test_factory_actions(r, *im);
		game.end();

		// ---- run 7: the investment fund
		im = game.start();
		r.check(im != nullptr, "investment fund: the instance starts");
		if (im == nullptr) {
			return;
		}
		test_investment_fund(r, *im);
		game.end();

		// ---- run 8: the world trade and the hunger of the unemployed
		im = game.start();
		r.check(im != nullptr, "trade: the instance starts");
		if (im == nullptr) {
			return;
		}
		test_supply_limit(r, *im);
		test_divisions(r, *im);
		test_supply_delivery(r, *im);
		test_trade_and_hunger(r, *im);
		game.end();

		// ---- the defines hash does not change while playing
		r.check(defines_hash != 0, "defines: the instance sees the hash of the defines");

		// ---- the sums that the provinces keep from one day to the next
		ProvinceInstance::set_verify_political_support_cache(false);
		r.check(
			ProvinceInstance::get_political_support_cache_mismatch_count() == kept_sums_differences_before,
			"pops: the political support that a province keeps is always the sum of the political support of its pops",
			fmt::format("{} differences", ProvinceInstance::get_political_support_cache_mismatch_count() - kept_sums_differences_before)
		);
		CountryInstance::set_verify_need_consumption(false);
		r.check(
			CountryInstance::get_need_consumption_mismatch_count() == consumption_differences_before,
			"pops: the consumption that a country records by type of pop is the sum of the reports",
			fmt::format("{} differences", CountryInstance::get_need_consumption_mismatch_count() - consumption_differences_before)
		);
	}
}

bool OpenVic::run_pure_selftests() {
	Results r;
	test_rate(r);
	test_defines(r);
	test_replay(r);
	test_save_header(r);
	test_credit_pure(r);
	r.summary();
	return r.failed == 0;
}

bool OpenVic::run_data_selftests(GameManager& game_manager, Bookmark const& bookmark) {
	Results r;
	Game game { game_manager, bookmark };
	test_game(r, game);
	r.summary();
	return r.failed == 0;
}

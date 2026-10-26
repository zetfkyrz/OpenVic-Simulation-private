#include "SelfTest.hpp"

#include <algorithm>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include <fmt/format.h>
#include <spdlog/spdlog.h>

#include <openvic-simulation/GameManager.hpp>
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

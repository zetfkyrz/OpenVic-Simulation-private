#include "SelfTest.hpp"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include <fmt/format.h>
#include <spdlog/spdlog.h>

#include <openvic-simulation/GameManager.hpp>
#include <openvic-simulation/InstanceManager.hpp>
#include <openvic-simulation/country/CountryInstance.hpp>
#include <openvic-simulation/defines/OpenVicDefines.hpp>
#include <openvic-simulation/misc/GameAction.hpp>
#include <openvic-simulation/misc/MoneyLedger.hpp>
#include <openvic-simulation/misc/Replay.hpp>
#include <openvic-simulation/misc/StateHash.hpp>

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
		entries.push_back({ 4, game_action_t(std::in_place_type<take_external_loan_argument_t>, country_index_t(5), country_index_t(6), fixed_point_t(-12) / 7, std::uint32_t(30)) });

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
		instance_manager.force_tick_and_update();
	}

	void play(InstanceManager& instance_manager, const int days) {
		for (int i = 0; i < days; ++i) {
			instance_manager.force_tick_and_update();
		}
	}

	std::string hex(const std::uint64_t value) {
		return Replay::hash_to_string(value);
	}

	void test_game(Results& r, Game& game) {
		constexpr int FIRST_DAYS = 40;
		constexpr int MORE_DAYS = 20;

		// ---- run 1: the game that the others are compared with
		InstanceManager* im = game.start();
		r.check(im != nullptr, "game: the first instance starts");
		if (im == nullptr) {
			return;
		}
		const std::uint64_t defines_hash = im->definition_manager.get_define_manager().get_openvic_defines().get_hash();
		const std::uint64_t start_hash = StateHash::compute(*im);
		give_orders(*im);
		play(*im, FIRST_DAYS);
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
		play(*im, FIRST_DAYS);
		const std::uint64_t hash_2 = StateHash::compute(*im);
		const std::string save_2 = Replay::serialise(im->get_action_history());
		r.check(hash_2 == hash_1, "determinism: two runs with the same actions end in the same state", hex(hash_1) + " != " + hex(hash_2));
		r.check(save_2 == save_1, "determinism: two runs write the same action history");
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
			+ flow(MoneyLedger::label_t::market_buyers_pay) + flow(MoneyLedger::label_t::factory_budget_floor);
		r.check(
			measured <= created + (1 << fixed_point_t::PRECISION),
			"money: the total grows only by the gold, the starting budgets of new factories and the rounding of the market",
			fmt::format("the total changed by {:.3f}, the allowed sources gave {:.3f}", pounds(measured), pounds(created))
		);
		fmt::println("TEST INFO money: the total changed by {:.1f}, created by the allowed sources {:.1f} (gold {:.1f}, starting budgets {:.1f})",
			pounds(measured), pounds(created), pounds(flow(MoneyLedger::label_t::gold_worker_pay) + flow(MoneyLedger::label_t::gold_income)),
			pounds(flow(MoneyLedger::label_t::factory_starting_budget)));
		const double rounding = pounds(flow(MoneyLedger::label_t::market_sellers_get) + flow(MoneyLedger::label_t::market_buyers_pay));
		const double volume = -pounds(flow(MoneyLedger::label_t::market_buyers_pay));
		r.check(rounding >= -1.0 && rounding <= volume * 0.001 + 1.0, "money: the rounding of the market is below 0.1% of the trade",
			fmt::format("rounding {:.3f} of {:.3f}", rounding, volume));
		ledger.disable();
		game.end();

		// ---- the defines hash does not change while playing
		r.check(defines_hash != 0, "defines: the instance sees the hash of the defines");
	}
}

bool OpenVic::run_pure_selftests() {
	Results r;
	test_rate(r);
	test_defines(r);
	test_replay(r);
	test_save_header(r);
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

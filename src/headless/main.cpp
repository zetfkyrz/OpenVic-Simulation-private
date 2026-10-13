#include <chrono>
#include <filesystem>
#include <memory>
#include <random>

#include <fmt/base.h>
#include <fmt/chrono.h>

#include <range/v3/view/enumerate.hpp>

#include <spdlog/common.h>
#include <spdlog/logger.h>
#include <spdlog/sinks/callback_sink.h>

#include <openvic-simulation/core/memory/MemoryTracker.hpp>
#include <openvic-simulation/core/memory/Vector.hpp>
#include <openvic-simulation/GameManager.hpp>
#include <openvic-simulation/country/CountryInstance.hpp>
#include <openvic-simulation/InstanceManager.hpp>
#include <openvic-simulation/economy/production/FactoryInstance.hpp>
#include <openvic-simulation/map/State.hpp>
#include <openvic-simulation/population/Pop.hpp>
#include <openvic-simulation/dataloader/Dataloader.hpp>
#include <openvic-simulation/economy/GoodDefinition.hpp>
#include <openvic-simulation/economy/production/ProductionType.hpp>
#include <openvic-simulation/economy/production/ResourceGatheringOperation.hpp>
#include <openvic-simulation/pathfinding/AStarPathing.hpp>
#include <openvic-simulation/testing/Testing.hpp>
#include <openvic-simulation/utility/Logger.hpp>

using namespace OpenVic;

inline static void print_memory_usage( //
	std::string_view prefix, std::source_location const& location = std::source_location::current()
) {
#ifdef DEBUG_ENABLED // memory tracking will return 0 without DEBUG_ENABLED
#if SPDLOG_ACTIVE_LEVEL <= SPDLOG_LEVEL_INFO
	spdlog::log(
#ifndef SPDLOG_NO_SOURCE_LOC
		spdlog::source_loc { location.file_name(), static_cast<int>(location.line()), location.function_name() },
#else
		spdlog::source_loc {},
#endif
		spdlog::level::info, "{} Memory Usage: {} Bytes", prefix, OpenVic::memory::MemoryTracker::get_memory_usage()
	);
#endif
#endif
}

// Days to simulate for the economy report (-e), 0 = no report
static size_t economy_report_days = 0;
// Index of the bookmark (start date) to simulate, 0 = 1836, 1 = 1861 (-k)
static size_t bookmark_index = 0;

// Summary of the factories and of the money in the economy, printed with the "ECON" prefix
static void print_economy_report(InstanceManager& instance_manager) {
	size_t factories = 0, open_factories = 0, building = 0, levels = 0;
	fixed_point_t budgets = 0, revenue = 0, wages = 0, owner_income = 0, profit = 0;
	double workforce_fraction_sum = 0;
	size_t staffed_factories = 0, over_threshold = 0;
	fixed_point_t expansion_funds = 0, total_cost = 0;

	for (StateSet const& state_set : instance_manager.get_map_instance().get_state_manager().get_state_sets()) {
		for (State const& state : state_set.get_states()) {
			for (FactoryInstance const& factory : state.get_factories()) {
				factories++;
				levels += static_cast<size_t>(type_safe::get(factory.get_level()));
				open_factories += factory.is_open();
				building += factory.is_under_construction();
				budgets += factory.get_budget();
				expansion_funds += factory.get_expansion_fund();
				total_cost += FactoryInstance::calculate_construction_cost(factory.building_type, instance_manager.get_market_instance());
				revenue += factory.get_revenue_yesterday();
				wages += factory.get_wages_yesterday();
				owner_income += factory.get_owner_income_yesterday();
				profit += factory.get_profit_yesterday();
				if (type_safe::get(factory.get_max_employee_count()) > 0) {
					staffed_factories++;
					over_threshold += type_safe::get(factory.get_employee_count()) * 100 >= type_safe::get(factory.get_max_employee_count()) * 80;
					workforce_fraction_sum += static_cast<double>(type_safe::get(factory.get_employee_count())) /
						static_cast<double>(type_safe::get(factory.get_max_employee_count()));
				}
			}
		}
	}

	fixed_point_t country_cash = 0, gold_income = 0;
	for (CountryInstance const& country : instance_manager.get_country_instance_manager().get_country_instances()) {
		country_cash += country.get_cash_stockpile().load();
		gold_income += country.get_gold_income_untracked();
	}
	fixed_point_t pop_cash = 0;
	for (ProvinceInstance const& province : instance_manager.get_map_instance().get_province_instances()) {
		for (Pop const& pop : province.get_pops()) {
			pop_cash += pop.get_cash().get_copy_of_value();
		}
	}

	fmt::println(
		"ECON {} factories={} open={} building={} levels={} employment={:.2f} revenue={:.1f} wages={:.1f} owners={:.1f} "
		"profit={:.1f} over80%={} funds={:.0f} avg_cost={:.0f} | money: countries={:.0f} pops={:.0f} factories={:.0f} total={:.0f}",
		instance_manager.get_today(), factories, open_factories, building, levels,
		staffed_factories > 0 ? workforce_fraction_sum / staffed_factories : 0.0, static_cast<double>(revenue),
		static_cast<double>(wages), static_cast<double>(owner_income), static_cast<double>(profit),
		over_threshold, static_cast<double>(expansion_funds), factories > 0 ? static_cast<double>(total_cost) / factories : 0.0,
		static_cast<double>(country_cash), static_cast<double>(pop_cash), static_cast<double>(budgets),
		static_cast<double>(country_cash + pop_cash + budgets)
	);
}

static void print_help(FILE* file, std::string_view program_name) {
	fmt::println(file, "Usage: {} [-h] [-t] [-e <days>] [-b <path>] [path]+", program_name);
	fmt::println(file, "    -e : Simulate the given number of days and print an economy report ('ECON') every 30 days.");
	fmt::println(file, "    -h : Print this help message and exit the program.");
	fmt::println(file, "    -t : Run tests after loading defines.");
	fmt::println(file, "    -b : Use the following path as the base directory (instead of searching for one).");
	fmt::println(file, "    -s : Use the following path as a hint to search for a base directory.");
	fmt::println(
		file,
		"Any following paths are read as mods (/path/to/my/MODNAME.mod), with priority starting at one above the base "
		"directory."
	);
	fmt::println(file, "(Paths with spaces need to be enclosed in \"quotes\").");
}

static void print_rgo(ProvinceInstance const& province) {
	ResourceGatheringOperation const& rgo = province.get_rgo();
	ProductionType const* const production_type_nullable = rgo.get_production_type_nullable();
	if (production_type_nullable == nullptr) {
		spdlog::error_s("\t{} - production_type: nullptr", province);
	}
#if SPDLOG_ACTIVE_LEVEL <= SPDLOG_LEVEL_INFO
	else {
		ProductionType const& production_type = *production_type_nullable;
		GoodDefinition const& output_good = production_type.output_good;

		SPDLOG_INFO("{}:", province);
		SPDLOG_INFO(
			"\tgood: {}, "
			"production_type: {}, "
			"size_multiplier: {:.3}, "
			"output_quantity_yesterday: {:.3}, "
			"revenue_yesterday: {:.3}, "
			"total owner income: {:.3}, "
			"total employee income: {:.3}",
			production_type.output_good, //
			production_type, //
			rgo.get_size_multiplier(), //
			rgo.get_output_quantity_yesterday(), //
			rgo.get_revenue_yesterday(), //
			rgo.get_total_owner_income_cache(), //
			rgo.get_total_employee_income_cache()
		);

		bool logged_employees = false;
		{
			pop_type_index_t pop_type_index {};
			for (const pop_size_t employees_of_type : rgo.get_employee_count_per_type_cache()) {
				if (employees_of_type > 0) {
					if (!logged_employees) {
						SPDLOG_INFO("\temployess:");
						logged_employees = true;
					}
					SPDLOG_INFO("\t\t{} {}", employees_of_type, pop_type_index);
				}
				++pop_type_index;
			}
		}
	}
#endif
}

using pathing_rng_t = std::mt19937;
using pathing_rng_seed_t = pathing_rng_t::result_type;

using testing_clock_t = std::chrono::high_resolution_clock;
using test_time_point_t = testing_clock_t::time_point;
using test_duration_t = testing_clock_t::duration;

template<size_t TestCount>
static test_duration_t run_pathing_test(AStarPathing& pathing, std::optional<pathing_rng_seed_t> seed) {
	PointMap const& points = pathing.get_point_map();

	if (!seed.has_value()) {
		thread_local std::random_device rd;
		seed = rd();

		SPDLOG_INFO("Running {} pathing tests with randomly generated seed: {}", TestCount, seed.value());
	} else {
		SPDLOG_INFO("Running {} pathing tests with fixed seed: {}", TestCount, seed.value());
	}

	thread_local pathing_rng_t gen { seed.value() };

	using it = decltype(points.points_map().end());
	std::array<std::pair<it, it>, TestCount> from_to_tests;
	std::array<memory::vector<PointMap::points_key_type>, TestCount> test_results;

	for (size_t index = 0; index < TestCount; index++) {
		it from = points.points_map().begin();
		it to = points.points_map().begin();
		std::uniform_int_distribution<> dis(0, points.get_point_count() - 1);
		std::advance(from, dis(gen));
		std::advance(to, dis(gen));
		from_to_tests[index] = { from, to };
	}

	const test_time_point_t start_time = testing_clock_t::now();
	for (auto [index, pair] : from_to_tests | ranges::views::enumerate) {
		test_results[index] = pathing.get_id_path(pair.first.key(), pair.second.key());
	}
	const test_time_point_t end_time = testing_clock_t::now();

	for (auto [index, test] : test_results | ranges::views::enumerate) {
		SPDLOG_INFO("-Start Test {}-", index + 1);
		SPDLOG_INFO("\tFrom: Id {}", from_to_tests[index].first.key());
		for (PointMap::points_key_type const& id : test) {
			if (id == test.back()) {
				break;
			}
			if (id == test.front()) {
				continue;
			}
			SPDLOG_INFO("\t\tId {}", id);
		}
		if (test.empty()) {
			SPDLOG_INFO("\t\tFailed to find path");
		}
		SPDLOG_INFO("\tTo: Id {}", from_to_tests[index].second.key());
		SPDLOG_INFO("-End Test {}-", index + 1);
	}

	return end_time - start_time;
}

struct SinkGuard {
	std::shared_ptr<spdlog::logger> logger;
	spdlog::sink_ptr sink;

	SinkGuard(std::shared_ptr<spdlog::logger> const& l, spdlog::sink_ptr s) : logger(l), sink(s) {
		logger->sinks().push_back(sink);
	}

	~SinkGuard() {
		std::vector<spdlog::sink_ptr>& sinks = logger->sinks();
		sinks.erase(std::remove(sinks.begin(), sinks.end(), sink), sinks.end());
	}
};

static size_t info_count = 0, warning_count = 0, error_count = 0, critical_count = 0;
static bool run_headless(fs::path const& root, memory::vector<memory::string>& mods, bool run_tests) {
	bool ret = true;
	Dataloader::path_vector_t roots = { root };
	Dataloader::path_vector_t replace_paths = {};

	spdlog::sink_ptr counter_sink = std::make_shared<spdlog::sinks::callback_sink_st>([](spdlog::details::log_msg const& msg) {
		switch (msg.level) {
			using namespace spdlog::level;
		case info:	   info_count++; break;
		case warn:	   warning_count++; break;
		case err:	   error_count++; break;
		case critical: critical_count++; break;
		default:	   break;
		}
	});

	SinkGuard guard(spdlog::default_logger(), counter_sink);

	GameManager game_manager {
		[] {
			SPDLOG_INFO("State updated");
		},
		nullptr, nullptr //
	};

	SPDLOG_INFO("Commit hash: {}", GameManager::get_commit_hash());

	SPDLOG_INFO("===== Setting base path... =====");
	ret &= game_manager.set_base_path(roots);

	SPDLOG_INFO("===== Loading mod descriptors... =====");
	ret &= game_manager.load_mod_descriptors();

	SPDLOG_INFO("===== Loading mods... =====");
	ret &= game_manager.load_mods(mods);

	SPDLOG_INFO("===== Loading definitions... =====");
	ret &= game_manager.load_definitions(
		[](std::string_view key, Dataloader::locale_t locale, std::string_view localisation) -> bool {
			return true;
		}
	);

	print_memory_usage("Definition Setup");

	if (run_tests) {
		// std::shared_ptr<spdlog::logger> testing_logger = spdlog::basic_logger_st(
		// 	"testing",
		// 	"testing/test_results/results.txt",
		// 	true
		// );
		// testing_logger->set_pattern("%v");
		std::shared_ptr<spdlog::logger> testing_logger = spdlog::default_logger();
		Testing testing { game_manager.get_definition_manager(), testing_logger };
		SPDLOG_INFO("Testing Loaded");
		testing.execute_all_scripts();
		testing.report_results();
		SPDLOG_INFO("Testing Executed");
	}

	SPDLOG_INFO("===== Setting up instance... =====");
	auto const& bookmarks = game_manager.get_definition_manager().get_history_manager().get_bookmark_manager().get_bookmarks();
	if (bookmark_index >= bookmarks.size()) {
		spdlog::error_s("Invalid bookmark index {}", bookmark_index);
		return false;
	}
	Bookmark const* const bookmark = &bookmarks[bookmark_index];
	ret &= game_manager.setup_instance(*bookmark);

	print_memory_usage("Instance Setup");

	SPDLOG_INFO("===== Starting game session... =====");
	ret &= game_manager.start_game_session();

	// This triggers a gamestate update
	ret &= game_manager.update_clock();

	print_memory_usage("Game Session Post-Start");

	// TODO - REMOVE TEST CODE
	SPDLOG_INFO("===== Ranking system test... =====");
	if (game_manager.get_instance_manager()) {
		const auto print_ranking_list = [ //
		](std::string_view title, OpenVic::forwardable_span<const std::reference_wrapper<CountryInstance>> countries) -> void {
			memory::string countries_str;
			for (CountryInstance& country : countries) {
				countries_str += fmt::format(
					"\n\t{} - Total #{} ({:.1}), Prestige #{} ({:.1}), Industry #{} ({:.1}), Military #{} ({:.1})", //
					country, //
					country.get_total_rank(), country.total_score.get_untracked(), //
					country.get_prestige_rank(), country.get_prestige_untracked(),
					country.get_industrial_rank(), country.get_industrial_power_untracked(),
					country.get_military_rank(), country.military_power.get_untracked()
				);
			}
			SPDLOG_INFO("{}:{}", title, countries_str);
		};

		CountryInstanceManager const& country_instance_manager = game_manager.get_instance_manager()->get_country_instance_manager();

		OpenVic::forwardable_span<const std::reference_wrapper<CountryInstance>> great_powers = country_instance_manager.get_great_powers();
		print_ranking_list("Great Powers", great_powers);
		print_ranking_list("Secondary Powers", country_instance_manager.get_secondary_powers());
		print_ranking_list("All countries", country_instance_manager.get_total_ranking());

		SPDLOG_INFO("===== RGO test... =====");
		for (size_t i = 0; i < std::min<size_t>(3, great_powers.size()); ++i) {
			CountryInstance const& great_power = great_powers[i];
			ProvinceInstance const* const capital_province = great_power.get_capital();
			if (capital_province == nullptr) {
				spdlog::warn_s("{} has no capital ProvinceInstance set.", great_power);
			} else {
				print_rgo(*capital_province);
			}
		}
	} else {
		spdlog::error_s("Instance manager not available!");
		ret = false;
	}

	if (ret) {
		static constexpr size_t TESTS = 10;
		// Set to std::nullopt to use a different random seed on each run
		static constexpr std::optional<pathing_rng_seed_t> LAND_SEED = 1836, SEA_SEED = 1861;

		using test_time_units_t = std::chrono::milliseconds;

		MapInstance& map_instance = game_manager.get_instance_manager()->get_map_instance();

		SPDLOG_INFO("===== Land Pathfinding test... =====");
		test_duration_t duration = std::chrono::duration_cast<test_time_units_t>( //
			run_pathing_test<TESTS>(map_instance.get_land_pathing(), LAND_SEED)
		);
		SPDLOG_INFO("Ran {} land pathing tests in {}", TESTS, duration);

		SPDLOG_INFO("===== Sea Pathfinding test... =====");
		duration = std::chrono::duration_cast<test_time_units_t>( //
			run_pathing_test<TESTS>(map_instance.get_sea_pathing(), SEA_SEED)
		);
		SPDLOG_INFO("Ran {} sea pathing tests in {}", TESTS, duration);
	}

	if (ret) {
		static constexpr size_t TICK_COUNT = 10;

		SPDLOG_INFO("===== Game Tick test... =====");
		size_t ticks_passed = 0;
		test_duration_t min_tick_duration = test_duration_t::max(), //
			max_tick_duration = test_duration_t::min(), //
			total_tick_duration {};
		const test_time_point_t start_time = testing_clock_t::now();
		while (++ticks_passed < TICK_COUNT) {
			const test_time_point_t tick_start = testing_clock_t::now();
			game_manager.get_instance_manager()->force_tick_and_update();
			const test_time_point_t tick_end = testing_clock_t::now();

			const test_duration_t tick_duration = tick_end - tick_start;

			min_tick_duration = std::min(min_tick_duration, tick_duration);
			max_tick_duration = std::max(max_tick_duration, tick_duration);

			total_tick_duration += tick_duration;

			print_memory_usage(fmt::format("Tick {} finished", ticks_passed));
		}
		const test_time_point_t end_time = testing_clock_t::now();
		const test_duration_t duration = end_time - start_time;

		if (ticks_passed != 0 && duration != test_duration_t::zero()) {
			const test_duration_t tick_tps = total_tick_duration / ticks_passed;
			const test_duration_t total_tps = duration / ticks_passed;
			spdlog::info(
				"Ran {} / {} ticks, total time {} at {} per tick, tick time only {} at {} per tick. "
				"Tick lengths ranged from {} to {}.",
				ticks_passed, TICK_COUNT, duration, total_tps, total_tick_duration, tick_tps, //
				min_tick_duration, max_tick_duration
			);
		} else {
			spdlog::error_s(
				"No ticks passed ({}, expected {}) or zero duration measured ({})!", ticks_passed, TICK_COUNT, duration
			);
			ret = false;
		}
	}

	if (ret && economy_report_days > 0) {
		InstanceManager& instance_manager = *game_manager.get_instance_manager();
		print_economy_report(instance_manager);
		for (size_t day = 1; day <= economy_report_days; ++day) {
			instance_manager.force_tick_and_update();
			if (day % 30 == 0) {
				print_economy_report(instance_manager);
			}
		}
	}

	SPDLOG_INFO("===== Ending game session... =====");
	ret &= game_manager.end_game_session();

	SPDLOG_INFO("Max Memory Usage: {} Bytes", OpenVic::memory::MemoryTracker::get_max_memory_usage());

	return ret;
}

/*
	$ program [-h] [-t] [-b] [path]+
*/

int main(int argc, char const* argv[]) {
	std::string_view program_name = get_filename(argc > 0 ? argv[0] : "", "<program>");
	fs::path root;
	memory::vector<memory::string> mods;
	mods.reserve(argc);
	bool run_tests = false;
	int argn = 0;

	/* Reads the next argument and converts it to a path via path_transform. If reading or converting fails, an error
	 * message and the help text are displayed, along with returning false to signify the program should exit.
	 */
	const auto _read = [&root, &argn, argc, argv, &program_name //
	](std::string_view command, std::string_view path_use, std::invocable<fs::path> auto path_transform) -> bool {
		if (root.empty()) {
			if (++argn < argc) {
				char const* path = argv[argn];
				root = path_transform(path);
				if (!root.empty()) {
					return true;
				} else {
					fmt::println(
						stderr, "Empty path after giving \"{}\" to {} command line argument \"{}\".", path, path_use, command
					);
				}
			} else {
				fmt::println(stderr, "Missing path after {} command line argument \"{}\".", path_use, command);
			}
		} else {
			fmt::println(stderr, "Duplicate {} command line argument \"-b\".", path_use);
		}
		print_help(stderr, program_name);
		return false;
	};

	while (++argn < argc) {
		char const* arg = argv[argn];
		if (strcmp(arg, "-h") == 0) {
			print_help(stdout, program_name);
			return 0;
		} else if (strcmp(arg, "-t") == 0) {
			run_tests = true;
		} else if (strcmp(arg, "-k") == 0) {
			if (++argn >= argc) {
				fmt::println(stderr, "Missing bookmark index after \"-k\".");
				return -1;
			}
			bookmark_index = static_cast<size_t>(std::max(0, std::atoi(argv[argn])));
		} else if (strcmp(arg, "-e") == 0) {
			if (++argn >= argc) {
				fmt::println(stderr, "Missing number of days after \"-e\".");
				return -1;
			}
			economy_report_days = static_cast<size_t>(std::max(0, std::atoi(argv[argn])));
		} else if (strcmp(arg, "-b") == 0) {
			if (!_read("-b", "base directory", std::identity {})) {
				return -1;
			}
		} else if (strcmp(arg, "-s") == 0) {
			if (!_read("-s", "search hint", Dataloader::search_for_game_path)) {
				return -1;
			}
		} else {
			break;
		}
	}
	if (root.empty()) {
		root = Dataloader::search_for_game_path();
		if (root.empty()) {
			fmt::println(stderr, "Search for base directory path failed!");
			print_help(stderr, program_name);
			return -1;
		}
	}
	while (argn < argc) {
		mods.emplace_back(argv[argn++]);
	}

	SPDLOG_INFO("!!! HEADLESS SIMULATION START !!!");

	const bool ret = run_headless(root, mods, run_tests);

	SPDLOG_INFO("!!! HEADLESS SIMULATION END !!!");

	spdlog::info("Load returned: {}", ret ? "SUCCESS" : "FAILURE");

	spdlog::info(
		"Logger Summary: "
#if SPDLOG_ACTIVE_LEVEL <= SPDLOG_LEVEL_INFO
		"Info = {}, "
#endif
		"Warning = {}, "
		"Error = {}, "
		"Critical = {}",
#if SPDLOG_ACTIVE_LEVEL <= SPDLOG_LEVEL_INFO
		info_count,
#endif
		warning_count, error_count, critical_count
	);

	return ret ? 0 : -1;
}

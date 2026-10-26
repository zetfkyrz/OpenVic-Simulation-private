#pragma once

#include <array>
#include <atomic>
#include <condition_variable>
#include <functional>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <thread>

#include "openvic-simulation/population/PopValuesFromProvince.hpp"
#include "openvic-simulation/core/memory/Vector.hpp"
#include "openvic-simulation/core/object/Date.hpp"
#include "openvic-simulation/core/portable/ForwardableSpan.hpp"
#include "openvic-simulation/core/random/RandomGenerator.hpp"
#include "openvic-simulation/population/PopValuesFromProvince.hpp"
#include "openvic-simulation/types/TypedIndices.hpp"

namespace OpenVic {
	struct GameRulesManager;
	struct GoodDefinition;
	struct GoodInstanceManager;
	struct CountryInstance;
	struct GoodInstance;
	struct ModifierEffectCache;
	struct PopsDefines;
	struct ProductionTypeManager;
	struct Strata;
	struct InstanceManager;
	struct MapInstance;
	
	//bundle work so they always have the same rng regardless of hardware concurrency
	struct WorkBundle {
	public:
		RandomU32 random_number_generator;
		forwardable_span<CountryInstance> countries_chunk;
		forwardable_span<GoodInstance> goods_chunk;
		forwardable_span<ProvinceInstance> provinces_chunk;

		constexpr WorkBundle() {}

		WorkBundle(
			RandomU32::state_type rng_state,
			forwardable_span<CountryInstance> new_countries_chunk,
			forwardable_span<GoodInstance> new_goods_chunk,
			forwardable_span<ProvinceInstance> new_provinces_chunk
		) : random_number_generator { rng_state },
			countries_chunk { new_countries_chunk },
			goods_chunk { new_goods_chunk },
			provinces_chunk { new_provinces_chunk }
			{}
	};

	struct ThreadPool {
	private:
		enum struct work_t : uint8_t {
			NONE,
			GOOD_EXECUTE_ORDERS,
			PROVINCE_INITIALISE_FOR_NEW_GAME,
			PROVINCE_TICK,
			COUNTRY_TICK_BEFORE_MAP,
			COUNTRY_TICK_AFTER_MAP,
			PROVINCE_UPDATE_GAMESTATE,
			COUNTRY_UPDATE_GAMESTATE,
			COUNTRY_CALLBACK
		};

		constexpr static std::size_t WORK_BUNDLE_COUNT = 32;
		std::array<WorkBundle, WORK_BUNDLE_COUNT> all_work_bundles;
		// The bundles are not fixed to a thread: every thread takes the next free bundle (the heaviest ones of the last time first), so
		// that a heavy bundle does not hold the others. The result does not depend on which thread works a bundle: the random numbers
		// and the data belong to the bundle.
		constexpr static std::size_t WORK_TYPE_COUNT = 16;
		std::atomic<std::size_t> next_claim = 0;
		std::array<std::size_t, WORK_BUNDLE_COUNT> claim_order {};
		std::array<std::array<std::uint64_t, WORK_BUNDLE_COUNT>, WORK_TYPE_COUNT> bundle_cost_ns {};
		memory::vector<std::thread> threads;
		memory::vector<work_t> work_per_thread;
		std::mutex thread_mutex, completed_mutex;
		std::condition_variable thread_condition, completed_condition;
		std::atomic<std::size_t> active_work_count = 0;
		bool is_cancellation_requested = false;
		Date const& current_date;
		// What the work of the update of the game state needs (set by the caller before the work starts, read by the workers)
		InstanceManager const* update_instance_manager = nullptr;
		MapInstance* update_map_instance = nullptr;
		std::function<void(CountryInstance&)> const* country_callback = nullptr;
		// The callback over the countries takes the countries one by one, the biggest ones (by the provinces they own) first
		forwardable_span<CountryInstance> all_countries;
		memory::vector<CountryInstance*> country_claim_order;
		std::atomic<std::size_t> next_country_claim = 0;

		inline static std::atomic<std::size_t> worker_thread_count_override = 0;

		void loop_until_cancelled(
			work_t& work_type,
			GameRulesManager const& game_rules_manager,
			GoodInstanceManager const& good_instance_manager,
			ModifierEffectCache const& modifier_effect_cache,
			PopsDefines const& pop_defines,
			ProductionTypeManager const& production_type_manager,
			forwardable_span<const CountryInstance> country_keys,
			const good_index_t good_count,
			const strata_index_t strata_count,
			forwardable_span<WorkBundle> work_bundles
		);
		void await_completion();
		void process_work(const work_t work_type);

	public:
		ThreadPool(Date const& new_current_date);
		~ThreadPool();

		// The number of worker threads of the pools that are created from now on (0: the number of the hardware threads). The result
		// of the simulation must not depend on it, because the work is always split into WORK_BUNDLE_COUNT bundles, each with a random
		// number generator of its own. The self tests and the headless option -j use it to check exactly that.
		static void set_worker_thread_count(const std::size_t count);

		void initialise_threadpool(
			GameRulesManager const& game_rules_manager,
			GoodInstanceManager const& good_instance_manager,
			ModifierEffectCache const& modifier_effect_cache,
			PopsDefines const& pop_defines,
			ProductionTypeManager const& production_type_manager,
			const strata_index_t strata_count,
			forwardable_span<GoodInstance> goods,
			forwardable_span<CountryInstance> countries,
			forwardable_span<ProvinceInstance> provinces
		);

		void process_good_execute_orders();
		void process_province_ticks();
		void process_province_initialise_for_new_game();
		void process_country_ticks_before_map();
		void process_country_ticks_after_map();
		// The provinces are independent in the update of the game state (each writes only itself, they only read the others): the pool
		// does it in parallel. is_initialised() is false before the pool has started (the caller then does the work itself).
		bool is_initialised() const {
			return !threads.empty();
		}
		void process_province_update_gamestate(InstanceManager const& instance_manager);
		// Every country works out its own values (and the flags of its own provinces): in parallel
		void process_country_update_gamestate(MapInstance& map_instance);
		// Calls the function for every country, the countries are shared among the threads (the function must only write what belongs to
		// its own country and must not depend on the order of the countries)
		void process_country_callback(std::function<void(CountryInstance&)> const& function);
	};
}
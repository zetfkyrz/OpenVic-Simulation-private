#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace OpenVic {
	/* Measures how long the parts of the simulation take (a tick, the update of the gamestate, the actions of the
	 * player) for the performance debugger of the game. Scopes nest, every scope is a line of the report with the path
	 * of its parents ("tick/market"). It only runs on the main thread (not inside the parallel market callbacks) and does
	 * not touch the state of the simulation, so it cannot change the results of the game. It does nothing while it is
	 * switched off. */
	struct TickProfiler {
		using clock_t = std::chrono::steady_clock;

		struct Entry {
			std::string path;
			std::string name;
			std::int32_t depth = 0;
			std::uint64_t last_ns = 0; // the last time the scope ran
			std::uint64_t max_ns = 0;
			std::uint64_t total_ns = 0;
			std::uint64_t calls = 0;
			double average_ns = 0.0; // moving average of one run
			// What it took in the current frame / tick, summed over all runs, reset by begin_cycle
			std::uint64_t cycle_ns = 0;
		};

		static constexpr std::size_t HISTORY_SIZE = 600;

	private:
		bool enabled = false;
		std::vector<Entry> entries;
		std::unordered_map<std::string, std::size_t> index_by_path;
		std::vector<std::size_t> stack;

		// Time of the whole ticks and updates, newest last
		std::array<float, HISTORY_SIZE> tick_history_ms {};
		std::array<float, HISTORY_SIZE> update_history_ms {};
		std::size_t tick_history_count = 0;
		std::size_t update_history_count = 0;

		std::size_t get_or_create(std::string_view name);

	public:
		struct Scope {
			TickProfiler* profiler = nullptr;
			std::size_t entry_index = 0;
			clock_t::time_point start;

			Scope() = default;
			Scope(TickProfiler* new_profiler, std::size_t new_entry_index);
			Scope(Scope&& other) noexcept;
			Scope(Scope const&) = delete;
			Scope& operator=(Scope const&) = delete;
			~Scope();
		};

		Scope scope(std::string_view name);

		void set_enabled(bool new_enabled);
		bool is_enabled() const {
			return enabled;
		}
		void reset();

		void record_tick(double milliseconds);
		void record_update(double milliseconds);

		std::vector<Entry> const& get_entries() const {
			return entries;
		}
		// Newest last, up to HISTORY_SIZE values
		std::vector<float> get_tick_history() const;
		std::vector<float> get_update_history() const;
	};
}

// A scope of the profiler that ends with the block it is declared in
#define OV_PROFILE_CONCAT_INNER(a, b) a##b
#define OV_PROFILE_CONCAT(a, b) OV_PROFILE_CONCAT_INNER(a, b)
#define OV_PROFILE_SCOPE(profiler, name) auto OV_PROFILE_CONCAT(ov_profile_scope_, __LINE__) = (profiler).scope(name)

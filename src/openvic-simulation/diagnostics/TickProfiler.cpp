#include "TickProfiler.hpp"

#include <algorithm>
#include <utility>

using namespace OpenVic;

TickProfiler::Scope::Scope(TickProfiler* new_profiler, const std::size_t new_entry_index)
	: profiler { new_profiler }, entry_index { new_entry_index }, start { clock_t::now() } {}

TickProfiler::Scope::Scope(Scope&& other) noexcept
	: profiler { std::exchange(other.profiler, nullptr) }, entry_index { other.entry_index }, start { other.start } {}

TickProfiler::Scope::~Scope() {
	if (profiler == nullptr) {
		return;
	}
	const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(clock_t::now() - start).count();
	const std::uint64_t ns = elapsed > 0 ? static_cast<std::uint64_t>(elapsed) : 0;

	if (!profiler->stack.empty()) {
		profiler->stack.pop_back();
	}
	if (entry_index < profiler->entries.size()) {
		Entry& entry = profiler->entries[entry_index];
		entry.last_ns = ns;
		entry.max_ns = std::max(entry.max_ns, ns);
		entry.total_ns += ns;
		entry.cycle_ns += ns;
		entry.calls++;
		entry.average_ns = entry.calls == 1 ? static_cast<double>(ns) : entry.average_ns * 0.9 + static_cast<double>(ns) * 0.1;
	}
}

std::size_t TickProfiler::get_or_create(const std::string_view name) {
	std::string path;
	if (!stack.empty()) {
		path = entries[stack.back()].path;
		path += '/';
	}
	path += name;

	const auto found = index_by_path.find(path);
	if (found != index_by_path.end()) {
		return found->second;
	}

	Entry entry;
	entry.path = path;
	entry.name = std::string { name };
	entry.depth = static_cast<std::int32_t>(stack.size());
	entries.push_back(std::move(entry));
	index_by_path.emplace(std::move(path), entries.size() - 1);
	return entries.size() - 1;
}

TickProfiler::Scope TickProfiler::scope(const std::string_view name) {
	if (!enabled) {
		return {};
	}
	const std::size_t index = get_or_create(name);
	stack.push_back(index);
	return Scope { this, index };
}

void TickProfiler::set_enabled(const bool new_enabled) {
	enabled = new_enabled;
	stack.clear();
}

void TickProfiler::reset() {
	entries.clear();
	index_by_path.clear();
	stack.clear();
	tick_history_ms.fill(0.0f);
	update_history_ms.fill(0.0f);
	tick_history_count = update_history_count = 0;
}

namespace {
	void push(std::array<float, TickProfiler::HISTORY_SIZE>& history, std::size_t& count, const float value) {
		if (count < history.size()) {
			history[count++] = value;
		} else {
			std::copy(history.begin() + 1, history.end(), history.begin());
			history.back() = value;
		}
	}
}

void TickProfiler::record_tick(const double milliseconds) {
	if (enabled) {
		push(tick_history_ms, tick_history_count, static_cast<float>(milliseconds));
	}
}

void TickProfiler::record_update(const double milliseconds) {
	if (enabled) {
		push(update_history_ms, update_history_count, static_cast<float>(milliseconds));
	}
}

std::vector<float> TickProfiler::get_tick_history() const {
	return { tick_history_ms.begin(), tick_history_ms.begin() + static_cast<std::ptrdiff_t>(tick_history_count) };
}

std::vector<float> TickProfiler::get_update_history() const {
	return { update_history_ms.begin(), update_history_ms.begin() + static_cast<std::ptrdiff_t>(update_history_count) };
}

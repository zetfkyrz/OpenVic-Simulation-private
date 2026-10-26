#include "OpenVicDefines.hpp"

#include <algorithm>
#include <bit>
#include <charconv>
#include <cmath>
#include <cstring>

#include <fmt/format.h>
#include <spdlog/spdlog.h>

#include "openvic-simulation/core/object/FixedPoint/String.hpp"
#include "openvic-simulation/utility/Logger.hpp"

using namespace OpenVic;
using namespace OpenVic::NodeTools;

namespace {
	constexpr std::uint64_t FNV_OFFSET = 14695981039346656037ULL;
	constexpr std::uint64_t FNV_PRIME = 1099511628211ULL;

	void hash_bytes(std::uint64_t& hash, void const* data, size_t size) {
		auto const* bytes = static_cast<unsigned char const*>(data);
		for (size_t i = 0; i < size; ++i) {
			hash ^= bytes[i];
			hash *= FNV_PRIME;
		}
	}

	template<typename T>
	void hash_value(std::uint64_t& hash, T value) {
		// Byte by byte in a fixed (little endian) order so that the hash is the same on every platform
		for (size_t i = 0; i < sizeof(T); ++i) {
			const unsigned char byte = static_cast<unsigned char>((static_cast<std::uint64_t>(value) >> (8 * i)) & 0xFF);
			hash_bytes(hash, &byte, 1);
		}
	}
}

void OpenVicDefineGroup::set_value(std::string_view file_name, std::string_view key, std::string_view text) {
	for (entry_t const& entry : entries) {
		if (entry.key != key) {
			continue;
		}

		double number = 0.0;
		const char* const begin = text.data();
		const char* const end = text.data() + text.size();
		if (!text.empty() && text.front() == '+') {
			text.remove_prefix(1);
		}
		const std::from_chars_result result = std::from_chars(text.data(), end, number);
		if (result.ec != std::errc {} || result.ptr != end || !std::isfinite(number)) {
			spdlog::warn_s("{}: value of {}.{} is not a number: \"{}\", the default is used", file_name, get_name(), key, std::string_view { begin, static_cast<size_t>(end - begin) });
			return;
		}
		if (number < entry.min || number > entry.max) {
			spdlog::warn_s(
				"{}: value of {}.{} is {}, outside of the allowed range {} to {}, the default is used", file_name, get_name(),
				key, number, entry.min, entry.max
			);
			return;
		}

		switch (entry.kind) {
		case kind_t::INT:
			if (number != std::floor(number)) {
				spdlog::warn_s("{}: value of {}.{} must be a whole number, got {}, the default is used", file_name, get_name(), key, number);
				return;
			}
			*static_cast<std::int32_t*>(entry.target) = static_cast<std::int32_t>(number);
			return;
		case kind_t::FIXED: {
			fixed_point_t value;
			const std::from_chars_result fixed_result = fp::from_chars_with_plus(value, text.data(), end);
			if (fixed_result.ec != std::errc {}) {
				spdlog::warn_s("{}: value of {}.{} does not fit a fixed point number: \"{}\", the default is used", file_name, get_name(), key, text);
				return;
			}
			*static_cast<fixed_point_t*>(entry.target) = value;
			return;
		}
		case kind_t::RATE: {
			const double scaled = number * Rate::DENOMINATOR;
			const double rounded = std::round(scaled);
			if (std::abs(scaled - rounded) > 1e-6) {
				spdlog::warn_s(
					"{}: value of {}.{} has more than 4 decimals, rounded to {}", file_name, get_name(), key,
					rounded / Rate::DENOMINATOR
				);
			}
			static_cast<Rate*>(entry.target)->numerator = static_cast<std::int32_t>(rounded);
			return;
		}
		case kind_t::DOUBLE:
			*static_cast<double*>(entry.target) = number;
			return;
		}
		return;
	}
	spdlog::warn_s("{}: unknown key {} in group {}", file_name, key, get_name());
}

void OpenVicDefineGroup::hash_into(std::uint64_t& hash) const {
	const std::string_view name = get_name();
	hash_bytes(hash, name.data(), name.size());
	for (entry_t const& entry : entries) {
		hash_bytes(hash, entry.key.data(), entry.key.size());
		switch (entry.kind) {
		case kind_t::INT:
			hash_value(hash, static_cast<std::uint32_t>(*static_cast<std::int32_t const*>(entry.target)));
			break;
		case kind_t::FIXED:
			hash_value(hash, static_cast<std::uint64_t>(static_cast<fixed_point_t const*>(entry.target)->get_raw_value()));
			break;
		case kind_t::RATE:
			hash_value(hash, static_cast<std::uint32_t>(static_cast<Rate const*>(entry.target)->numerator));
			break;
		case kind_t::DOUBLE:
			hash_value(hash, std::bit_cast<std::uint64_t>(*static_cast<double const*>(entry.target)));
			break;
		}
	}
}

OpenVicDefines::OpenVicDefines() {
	update_hash();
}

std::vector<OpenVicDefineGroup*> OpenVicDefines::get_groups() {
	return { &ai, &population, &politics, &colonisation, &war, &peace, &research, &economy, &credit, &trade };
}

std::vector<OpenVicDefineGroup const*> OpenVicDefines::get_groups() const {
	return { &ai, &population, &politics, &colonisation, &war, &peace, &research, &economy, &credit, &trade };
}

void OpenVicDefines::update_hash() {
	hash = FNV_OFFSET;
	for (OpenVicDefineGroup const* group : get_groups()) {
		group->hash_into(hash);
	}
}

bool OpenVicDefines::load_file(ast::NodeCPtr root, std::string_view file_name) {
	file_loaded = true;
	std::vector<OpenVicDefineGroup*> groups = get_groups();

	// The result of the parser is ignored on purpose: a broken file must not stop the game, only warn.
	expect_dictionary_keys(
		"defines", ZERO_OR_ONE,
		expect_dictionary([&](std::string_view group_name, ast::NodeCPtr group_node) -> bool {
			const auto it = std::find_if(groups.begin(), groups.end(), [&](OpenVicDefineGroup* group) {
				return group->get_name() == group_name;
			});
			if (it == groups.end()) {
				// start_date, end_date and the groups of defines.lua are not ours
				if (group_name != "start_date" && group_name != "end_date") {
					spdlog::warn_s("{}: unknown group {}", file_name, group_name);
				}
				return true;
			}
			OpenVicDefineGroup* const group = *it;
			expect_dictionary([&](std::string_view key, ast::NodeCPtr value_node) -> bool {
				expect_identifier_or_string([&](std::string_view text) -> bool {
					group->set_value(file_name, key, text);
					return true;
				})(value_node);
				return true;
			})(group_node);
			return true;
		})
	)(root);

	update_hash();
	SPDLOG_INFO("Loaded {}, defines hash {:016x}", file_name, hash);
	return true;
}

std::string OpenVicDefines::to_lua_text() const {
	std::string text =
		"-- Example of common/openvic_defines.lua: all the parameters of the OpenVic additions with their default values.\n"
		"-- Every key is optional; a missing key keeps the default. Rates are fractions (0.25 = 25 per cent) with at most\n"
		"-- 4 decimals. A wrong key or value is reported in the log and the default is used. Copy the groups you change into\n"
		"-- the common/openvic_defines.lua of your mod (the first file found by the game is used, it is not merged).\n"
		"-- Generated by: openvic-simulation.headless -D <file>\n\n"
		"defines = {\n";
	for (OpenVicDefineGroup const* group : get_groups()) {
		text += "\t" + std::string { group->get_name() } + " = {\n";
		for (OpenVicDefineGroup::entry_t const& entry : group->get_entries()) {
			std::string value;
			switch (entry.kind) {
			case OpenVicDefineGroup::kind_t::INT:
				value = std::to_string(*static_cast<std::int32_t const*>(entry.target));
				break;
			case OpenVicDefineGroup::kind_t::FIXED:
				value = fmt::format("{}", *static_cast<fixed_point_t const*>(entry.target));
				break;
			case OpenVicDefineGroup::kind_t::RATE: {
				const std::int32_t numerator = static_cast<Rate const*>(entry.target)->numerator;
				value = fmt::format("{}", static_cast<double>(numerator) / Rate::DENOMINATOR);
				break;
			}
			case OpenVicDefineGroup::kind_t::DOUBLE:
				value = fmt::format("{}", *static_cast<double const*>(entry.target));
				break;
			}
			const char* const kind_name = entry.kind == OpenVicDefineGroup::kind_t::INT ? "whole number"
				: entry.kind == OpenVicDefineGroup::kind_t::RATE ? "fraction" : "number";
			text += fmt::format("\t\t{} = {}, -- {} {} to {}\n", entry.key, value, kind_name, entry.min, entry.max);
		}
		text += "\t},\n";
	}
	text += "}\n";
	return text;
}

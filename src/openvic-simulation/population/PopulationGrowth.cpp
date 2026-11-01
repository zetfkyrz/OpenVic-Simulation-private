#include "PopulationGrowth.hpp"

#include <algorithm>
#include <fstream>
#include <string>

#include "openvic-simulation/defines/OpenVicDefines.hpp"
#include "openvic-simulation/population/Culture.hpp"
#include "openvic-simulation/utility/Logger.hpp"

using namespace OpenVic;
using namespace OpenVic::NodeTools;

namespace {
	std::size_t line_of(std::string_view source, std::string_view key) {
		std::ifstream file { std::string { source } };
		std::string text;
		std::size_t line_number = 0;
		while (std::getline(file, text)) {
			line_number++;
			if (!key.empty() && text.find(key) != std::string::npos) {
				return line_number;
			}
		}
		return 0;
	}
}

fixed_point_t PopulationGrowthRule::rate_in(const std::int32_t year) const {
	if (periods.empty()) {
		return fixed_point_t::_0;
	}
	fixed_point_t rate = periods.front().second;
	for (auto const& [from_year, period_rate] : periods) {
		if (from_year <= year) {
			rate = period_rate;
		}
	}
	return rate;
}

std::size_t PopulationGrowthData::load_file(std::string_view source, ast::NodeCPtr root, CultureManager const& culture_manager) {
	std::size_t loaded = 0;
	const bool parsed = expect_dictionary_keys_and_default(
		[source](std::string_view key, ast::NodeCPtr) -> bool {
			spdlog::warn_s("Unknown key \"{}\" in the population growth {} ({}:{})", key, source, source, line_of(source, key));
			return true;
		},
		"default", ZERO_OR_ONE, expect_fixed_point([this](fixed_point_t rate) -> bool {
			default_rate = rate;
			has_default = true;
			return true;
		}),
		"rule", ZERO_OR_MORE, [this, source, &culture_manager, &loaded](ast::NodeCPtr node) -> bool {
			PopulationGrowthRule rule;
			rule.source = source;
			bool has_yearly = false;
			fixed_point_t yearly;
			const bool ok = expect_dictionary_keys_and_default(
				[source](std::string_view key, ast::NodeCPtr) -> bool {
					spdlog::warn_s("Unknown key \"{}\" in a rule of the population growth {} ({}:{})", key, source, source, line_of(source, key));
					return true;
				},
				"tag", ZERO_OR_ONE, expect_identifier_or_string(assign_variable_callback_string(rule.tag)),
				"group", ZERO_OR_ONE, expect_identifier_or_string(assign_variable_callback_string(rule.group)),
				"culture", ZERO_OR_ONE, expect_identifier_or_string(assign_variable_callback_string(rule.culture)),
				"region", ZERO_OR_ONE, expect_identifier_or_string(assign_variable_callback_string(rule.region)),
				"yearly", ZERO_OR_ONE, expect_fixed_point([&yearly, &has_yearly](fixed_point_t rate) -> bool {
					yearly = rate;
					has_yearly = true;
					return true;
				}),
				"periods", ZERO_OR_ONE, expect_dictionary([&rule](std::string_view year_text, ast::NodeCPtr value) -> bool {
					std::int32_t year = 0;
					const std::string text { year_text };
					char* end = nullptr;
					year = static_cast<std::int32_t>(std::strtol(text.c_str(), &end, 10));
					if (end == text.c_str() || *end != '\0') {
						spdlog::warn_s("\"{}\" is not a year in the periods of a rule of the population growth", year_text);
						return false;
					}
					fixed_point_t rate;
					const bool rate_ok = expect_fixed_point(assign_variable_callback(rate))(value);
					rule.periods.emplace_back(year, rate);
					return rate_ok;
				})
			)(node);
			const auto skip = [&](std::string_view reason) -> bool {
				spdlog::warn_s("A rule of the population growth skipped: {} ({}:{})", reason, source, line_of(source, "rule"));
				return true;
			};
			if (!ok) {
				return skip("it could not be read, see the messages above");
			}
			if (has_yearly == !rule.periods.empty()) {
				return skip("a rule has either yearly or periods");
			}
			if (has_yearly) {
				rule.periods.emplace_back(std::numeric_limits<std::int32_t>::min(), yearly);
			}
			std::sort(rule.periods.begin(), rule.periods.end(), [](auto const& a, auto const& b) { return a.first < b.first; });
			if (rule.specificity() == 0) {
				return skip("a rule names none of tag, group, culture and region (use default)");
			}
			if (!rule.group.empty() && culture_manager.get_culture_group_by_identifier(rule.group) == nullptr) {
				return skip(memory::fmt::format("the culture group \"{}\" does not exist", rule.group));
			}
			if (!rule.culture.empty() && culture_manager.get_culture_by_identifier(rule.culture) == nullptr) {
				return skip(memory::fmt::format("the culture \"{}\" does not exist", rule.culture));
			}
			rules.push_back(std::move(rule));
			loaded++;
			return true;
		}
	)(root);
	if (!parsed) {
		spdlog::warn_s("The population growth file {} is broken, what could be read is used", source);
	}
	return loaded;
}

fixed_point_t PopulationGrowthData::get_yearly_rate(
	std::string_view tag, std::string_view group, std::string_view culture, std::string_view region, const std::int32_t year, const fixed_point_t fallback
) const {
	PopulationGrowthRule const* best = nullptr;
	for (PopulationGrowthRule const& rule : rules) {
		if ((!rule.tag.empty() && rule.tag != tag) || (!rule.group.empty() && rule.group != group) || (!rule.culture.empty() && rule.culture != culture)
			|| (!rule.region.empty() && rule.region != region)) {
			continue;
		}
		if (best == nullptr || rule.specificity() >= best->specificity()) {
			best = &rule;
		}
	}
	if (best != nullptr) {
		return best->rate_in(year);
	}
	return has_default ? default_rate : fallback;
}

fixed_point_t population_growth::monthly_yearly_rate(
	OpenVicPopulationDefines const& defines, const fixed_point_t base_yearly, const fixed_point_t life_needs, const fixed_point_t growth_multiplier
) {
	const fixed_point_t breakeven = defines.growth_breakeven_life_needs.fixed();
	fixed_point_t rate = fixed_point_t::_0;
	if (base_yearly < fixed_point_t::_0) {
		rate = base_yearly; // a famine of the data
	}
	if (life_needs >= breakeven) {
		if (base_yearly > fixed_point_t::_0) {
			const fixed_point_t full = std::max(breakeven + fixed_point_t(1) / 100, defines.growth_full_life_needs.fixed());
			const fixed_point_t share = std::min(fixed_point_t::_1, (life_needs - breakeven) / (full - breakeven));
			rate = base_yearly * share * growth_multiplier;
		}
	} else {
		rate += defines.decline_per_year.of(-(breakeven - life_needs) / breakeven);
	}
	return rate;
}

fixed_point_t population_growth::growth_multiplier(
	OpenVicPopulationDefines const& defines, const fixed_point_t population_growth_pct, const fixed_point_t casualties_taken_product
) {
	const fixed_point_t sanitation = std::max(fixed_point_t::_0, fixed_point_t::_1 + population_growth_pct);
	const fixed_point_t medicine = std::max(fixed_point_t::_0, fixed_point_t::_1 + defines.medicine_growth_weight.of(fixed_point_t::_1 - casualties_taken_product));
	return sanitation * medicine;
}

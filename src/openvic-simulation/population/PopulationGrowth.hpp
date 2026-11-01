#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <utility>

#include "openvic-simulation/core/memory/String.hpp"
#include "openvic-simulation/core/memory/Vector.hpp"
#include "openvic-simulation/core/object/FixedPoint.hpp"
#include "openvic-simulation/dataloader/NodeTools.hpp"

namespace OpenVic {
	struct CultureManager;
	struct OpenVicPopulationDefines;

	// The natural growth of the population in the data (docs/POPULATION.md, A2): ov_data/common/ov_population/growth.txt
	//
	//   default = 0.007                       # the yearly growth of a pop that no rule is for (0.007 = 0.7 % a year)
	//   rule = { tag = ENG yearly = 0.012 }   # the pops of the country
	//   rule = { group = germanic yearly = 0.010 }                  # the pops of a culture group
	//   rule = { tag = JAP periods = { 1836 = 0.0015 1868 = 0.007 } }   # from the year on (the last period goes on)
	//   rule = { tag = ENG culture = irish periods = { 1836 = 0.012 1846 = -0.02 1852 = 0.003 } }   # a culture inside a country
	//   rule = { region = ENG_263 yearly = 0.01 }                   # a region of the map (map/region.txt)
	//
	// A rule holds for a pop when all the conditions it names hold; of the rules that hold the most specific one counts (region 8, culture 4, tag 2,
	// group 1 are added up; the later one in the files wins a tie, so a mod can override). Mods override the file as a whole.
	struct PopulationGrowthRule {
		memory::string tag, group, culture, region; // "" : any
		memory::vector<std::pair<std::int32_t, fixed_point_t>> periods; // the year the rate begins in, sorted
		memory::string source;

		[[nodiscard]] int specificity() const {
			return (region.empty() ? 0 : 8) + (culture.empty() ? 0 : 4) + (tag.empty() ? 0 : 2) + (group.empty() ? 0 : 1);
		}
		[[nodiscard]] fixed_point_t rate_in(std::int32_t year) const;
	};

	struct PopulationGrowthData {
	private:
		memory::vector<PopulationGrowthRule> rules;
		bool has_default = false;
		fixed_point_t default_rate;

	public:
		// A broken rule is skipped with a warning (file, line, reason); returns the number of rules loaded
		std::size_t load_file(std::string_view source, ast::NodeCPtr root, CultureManager const& culture_manager);

		[[nodiscard]] bool is_loaded() const {
			return has_default || !rules.empty();
		}
		[[nodiscard]] std::size_t rule_count() const {
			return rules.size();
		}
		// The yearly growth of a pop: the rule that holds and is the most specific, else the default of the file, else fallback
		[[nodiscard]] fixed_point_t get_yearly_rate(
			std::string_view tag, std::string_view group, std::string_view culture, std::string_view region, std::int32_t year, fixed_point_t fallback
		) const;
	};

	namespace population_growth {
		// The yearly growth of a pop in the month, from the base of the data: a pop whose life needs are met above the breakeven grows by the base times the
		// share of the way from the breakeven to the full life needs; below it the pop shrinks (decline of the defines times the share of the way down). A negative
		// base (a famine of the data) always counts. The modifiers of the country (medicine: the casualties; the sanitation and the technologies of the health:
		// population_growth_pct) multiply the growth, never the decline.
		[[nodiscard]] fixed_point_t monthly_yearly_rate(
			OpenVicPopulationDefines const& defines, fixed_point_t base_yearly, fixed_point_t life_needs, fixed_point_t growth_multiplier
		);
		// The multiplier of the growth of a country: (1 + population_growth_pct) times (1 + the weight of the medicine times the casualties taken less than 1)
		[[nodiscard]] fixed_point_t growth_multiplier(
			OpenVicPopulationDefines const& defines, fixed_point_t population_growth_pct, fixed_point_t casualties_taken_product
		);
	}
}

#pragma once

#include <cstdint>
#include <string_view>
#include <vector>

#include "openvic-simulation/core/object/FixedPoint.hpp"
#include "openvic-simulation/dataloader/NodeTools.hpp"

namespace OpenVic {
	/* Tunable parameters of the OpenVic additions to the simulation (AI, population dynamics, politics, wars, ...).
	 *
	 * They are read from the optional file common/openvic_defines.lua, which uses the syntax of defines.lua:
	 *   defines = { ov_ai = { WAR_CHANCE_PER_YEAR = 0.25, ... }, ov_war = { ... } }
	 * A missing file, group or key keeps the built-in default (the value the code used before it became configurable).
	 * Unknown keys, values that are not numbers and values outside of the allowed range produce a warning with the file
	 * name and key, and the default is used. Mods override the file the usual way (the first file found by the
	 * dataloader is used). Rates are fractions (0.25 and not 25 percent).
	 *
	 * Each group is declared by an X-macro list: INT(KEY, member, default, min, max), FIXED(...) for fixed_point_t,
	 * RATE(...) for a Rate (fraction, bit exact, default Rate(25, 100)) and DOUBLE(...) for double. Add a parameter by adding one line to the list of its group. */
	/* A fraction stored as a whole number of 1/10000, so that "x * numerator / denominator" of the original constants
	 * (for example x * 25 / 1000) gives exactly the same fixed point number as before they became configurable. The file
	 * holds the fraction (0.025), at most 4 decimals. */
	struct Rate {
		static constexpr std::int32_t DENOMINATOR = 10000;
		std::int32_t numerator = 0;

		constexpr Rate() = default;
		// numerator / denominator, the denominator must divide 10000
		constexpr Rate(std::int32_t new_numerator, std::int32_t new_denominator)
			: numerator { new_numerator * (DENOMINATOR / new_denominator) } {
			static_assert(DENOMINATOR % 1000 == 0);
		}
		// x * rate, same as x * numerator / denominator
		fixed_point_t of(fixed_point_t const x) const {
			return x * numerator / DENOMINATOR;
		}
		// The rate as a number, same as fixed_point_t(numerator) / denominator
		fixed_point_t fixed() const {
			return fixed_point_t(numerator) / DENOMINATOR;
		}
		constexpr double as_double() const {
			return static_cast<double>(numerator) / DENOMINATOR;
		}
		// Whole numbers (people, days) times the rate, rounded down
		constexpr std::int64_t of_int(std::int64_t const x) const {
			return x * numerator / DENOMINATOR;
		}
	};

	struct OpenVicDefineGroup {
		enum struct kind_t : std::uint8_t { INT, FIXED, RATE, DOUBLE };

		struct entry_t {
			std::string_view key;
			kind_t kind;
			void* target;
			double min;
			double max;
		};

	protected:
		std::vector<entry_t> entries;

		OpenVicDefineGroup() = default;

	public:
		OpenVicDefineGroup(OpenVicDefineGroup const&) = delete;
		OpenVicDefineGroup& operator=(OpenVicDefineGroup const&) = delete;
		virtual ~OpenVicDefineGroup() = default;

		virtual std::string_view get_name() const = 0;
		std::vector<entry_t> const& get_entries() const {
			return entries;
		}

		// Parses one value of the group, warns and keeps the old value if it is invalid
		void set_value(std::string_view file_name, std::string_view key, std::string_view text);
		// Mixes the keys and the values into a 64 bit FNV-1a hash
		void hash_into(std::uint64_t& hash) const;
	};

#define OV_DEFINE_DECLARE_INT(KEY, member, def, lo, hi) std::int32_t member = def;
#define OV_DEFINE_DECLARE_FIXED(KEY, member, def, lo, hi) fixed_point_t member = def;
#define OV_DEFINE_DECLARE_RATE(KEY, member, def, lo, hi) Rate member = def;
#define OV_DEFINE_DECLARE_DOUBLE(KEY, member, def, lo, hi) double member = def;
#define OV_DEFINE_REGISTER_INT(KEY, member, def, lo, hi) \
	entries.push_back({ #KEY, kind_t::INT, &member, static_cast<double>(lo), static_cast<double>(hi) });
#define OV_DEFINE_REGISTER_FIXED(KEY, member, def, lo, hi) \
	entries.push_back({ #KEY, kind_t::FIXED, &member, static_cast<double>(lo), static_cast<double>(hi) });
#define OV_DEFINE_REGISTER_RATE(KEY, member, def, lo, hi) \
	entries.push_back({ #KEY, kind_t::RATE, &member, static_cast<double>(lo), static_cast<double>(hi) });
#define OV_DEFINE_REGISTER_DOUBLE(KEY, member, def, lo, hi) \
	entries.push_back({ #KEY, kind_t::DOUBLE, &member, static_cast<double>(lo), static_cast<double>(hi) });

#define OV_DEFINE_GROUP(StructName, group_name, LIST) \
	struct StructName final : OpenVicDefineGroup { \
		LIST(OV_DEFINE_DECLARE_INT, OV_DEFINE_DECLARE_FIXED, OV_DEFINE_DECLARE_RATE, OV_DEFINE_DECLARE_DOUBLE) \
		StructName() { \
			LIST(OV_DEFINE_REGISTER_INT, OV_DEFINE_REGISTER_FIXED, OV_DEFINE_REGISTER_RATE, OV_DEFINE_REGISTER_DOUBLE) \
		} \
		std::string_view get_name() const override { \
			return group_name; \
		} \
	};

	// ----- The groups. One line per parameter: KEY, member, default, minimum, maximum -----

#define OV_AI_DEFINES(INT, FIXED, RATE, DOUBLE) \
	RATE(WAR_CHANCE_PER_YEAR, war_chance_per_year, Rate(25, 100), 0, 1) \
	RATE(MAX_TARGET_POWER_RATIO, max_target_power_ratio, Rate(60, 100), 0, 100) \
	RATE(MAX_TARGET_POWER_RATIO_PLAYER, max_target_power_ratio_player, Rate(30, 100), 0, 100) \
	INT(MAX_RANK_FOR_WARS, max_rank_for_wars, 30, 0, 1000) \
	INT(WAR_FREE_UNTIL_YEAR, war_free_until_year, 1836, 0, 3000) \
	INT(WAR_FREE_UNTIL_MONTH, war_free_until_month, 7, 0, 12) \
	INT(WAR_DECISION_SALT, war_decision_salt, 7, 0, 1000000) \
	INT(FACTORY_PAYBACK_YEARS, factory_payback_years, 8, 1, 100) \
	INT(FACTORY_CASH_COST_MULTIPLE, factory_cash_cost_multiple, 3, 1, 100) \
	INT(EXPECTED_WAGE_DIVISOR, expected_wage_divisor, 2, 1, 100) \
	RATE(WAR_REGIMENTS_PER_PROVINCE, war_regiments_per_province, Rate(300, 1000), 0, 100) \
	INT(MIN_WAR_REGIMENTS, min_war_regiments, 3, 0, 1000) \
	INT(MAX_RECRUITS_PER_MONTH, max_recruits_per_month, 3, 0, 1000) \
	INT(COLONISE_MIN_POINTS_DIVISOR, colonise_min_points_divisor, 2, 1, 100)
#define OV_POPULATION_DEFINES(INT, FIXED, RATE, DOUBLE) \
	RATE(GROWTH_BREAKEVEN_LIFE_NEEDS, growth_breakeven_life_needs, Rate(35, 100), 0, 1) \
	RATE(GROWTH_PER_YEAR, growth_per_year, Rate(25, 1000), 0, 10) \
	RATE(DECLINE_PER_YEAR, decline_per_year, Rate(60, 1000), 0, 10) \
	RATE(ASSIMILATION_PER_MONTH, assimilation_per_month, Rate(1, 1000), 0, 1) \
	INT(ASSIMILATION_SAME_GROUP_MULTIPLIER, assimilation_same_group_multiplier, 2, 1, 100) \
	RATE(PROMOTION_PER_MONTH, promotion_per_month, Rate(4, 100), 0, 1) \
	RATE(CRAFTSMEN_DEMOTION_UNEMPLOYMENT, craftsmen_demotion_unemployment, Rate(1, 4), 0, 1) \
	RATE(PEASANTS_PROMOTION_UNEMPLOYMENT, peasants_promotion_unemployment, Rate(15, 100), 0, 1) \
	RATE(MIGRATION_PER_MONTH, migration_per_month, Rate(3, 1000), 0, 1) \
	RATE(MIGRATION_SCORE_MARGIN, migration_score_margin, Rate(8, 100), 0, 1) \
	RATE(MIGRATION_UNEMPLOYMENT_THRESHOLD, migration_unemployment_threshold, Rate(1, 5), 0, 1) \
	RATE(MIGRATION_LIFE_NEEDS_THRESHOLD, migration_life_needs_threshold, Rate(1, 2), 0, 1) \
	RATE(SCORE_UNEMPLOYMENT_WEIGHT, score_unemployment_weight, Rate(1, 2), 0, 1) \
	RATE(SCORE_LIFE_NEEDS_WEIGHT, score_life_needs_weight, Rate(1, 2), 0, 1)
#define OV_POLITICS_DEFINES(INT, FIXED, RATE, DOUBLE)
#define OV_COLONISATION_DEFINES(INT, FIXED, RATE, DOUBLE)
#define OV_WAR_DEFINES(INT, FIXED, RATE, DOUBLE)
#define OV_PEACE_DEFINES(INT, FIXED, RATE, DOUBLE)
#define OV_RESEARCH_DEFINES(INT, FIXED, RATE, DOUBLE)
#define OV_ECONOMY_DEFINES(INT, FIXED, RATE, DOUBLE)
	// Placeholder for the credit system rework: nothing reads these yet, no logic is attached to them. 0 = not set.
#define OV_CREDIT_DEFINES(INT, FIXED, RATE, DOUBLE) \
	INT(LOAN_TERM_MIN_DAYS, loan_term_min_days, 0, 0, 36500) \
	INT(LOAN_TERM_MAX_DAYS, loan_term_max_days, 0, 0, 36500) \
	INT(PAYOUT_DELAY_DAYS, payout_delay_days, 0, 0, 36500) \
	RATE(INTEREST_MIN, interest_min, Rate(0, 1), 0, 100) \
	RATE(INTEREST_MAX, interest_max, Rate(0, 1), 0, 100)

	OV_DEFINE_GROUP(OpenVicAIDefines, "ov_ai", OV_AI_DEFINES)
	OV_DEFINE_GROUP(OpenVicPopulationDefines, "ov_population", OV_POPULATION_DEFINES)
	OV_DEFINE_GROUP(OpenVicPoliticsDefines, "ov_politics", OV_POLITICS_DEFINES)
	OV_DEFINE_GROUP(OpenVicColonisationDefines, "ov_colonisation", OV_COLONISATION_DEFINES)
	OV_DEFINE_GROUP(OpenVicWarDefines, "ov_war", OV_WAR_DEFINES)
	OV_DEFINE_GROUP(OpenVicPeaceDefines, "ov_peace", OV_PEACE_DEFINES)
	OV_DEFINE_GROUP(OpenVicResearchDefines, "ov_research", OV_RESEARCH_DEFINES)
	OV_DEFINE_GROUP(OpenVicEconomyDefines, "ov_economy", OV_ECONOMY_DEFINES)
	OV_DEFINE_GROUP(OpenVicCreditDefines, "ov_credit", OV_CREDIT_DEFINES)

	struct OpenVicDefines {
	private:
		std::uint64_t hash = 0;
		bool file_loaded = false;

		void update_hash();

	public:
		OpenVicAIDefines ai;
		OpenVicPopulationDefines population;
		OpenVicPoliticsDefines politics;
		OpenVicColonisationDefines colonisation;
		OpenVicWarDefines war;
		OpenVicPeaceDefines peace;
		OpenVicResearchDefines research;
		OpenVicEconomyDefines economy;
		OpenVicCreditDefines credit;

		OpenVicDefines();

		// Reads common/openvic_defines.lua (already parsed to root). Never fails: problems are warnings, defaults stay.
		bool load_file(ast::NodeCPtr root, std::string_view file_name);

		// Deterministic hash of all final values (after the mods), stored in save games and compared on loading
		std::uint64_t get_hash() const {
			return hash;
		}
		bool is_file_loaded() const {
			return file_loaded;
		}
		// The groups in a fixed order
		std::vector<OpenVicDefineGroup*> get_groups();
		std::vector<OpenVicDefineGroup const*> get_groups() const;
	};
}

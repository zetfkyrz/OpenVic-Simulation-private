#pragma once

#include <cstdint>
#include <string>
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
	/* An AI country at war recruits whole divisions by its first template (docs/RECRUITMENT.md): at most AI_DIVISIONS_PER_MONTH a month, in its */ \
	/* capital first, then in its other provinces; when a division cannot be recruited (the manpower, the goods) it recruits single regiments as */ \
	/* before (MAX_RECRUITS_PER_MONTH). 0 turns the divisions off */ \
	INT(AI_DIVISIONS_PER_MONTH, ai_divisions_per_month, 1, 0, 100) \
	INT(COLONISE_MIN_POINTS_DIVISOR, colonise_min_points_divisor, 2, 1, 100) \
	/* The budget of an AI country, once a month (docs/POPULATION.md A2-b): when the treasury holds less than BUDGET_RESERVE_DAYS days of the spending */ \
	/* and the income of yesterday is under the spending, the taxes of all strata go up by BUDGET_STEP (to BUDGET_MAX_TAX), and when they are at the top the */ \
	/* military, administration and education spending go down by BUDGET_STEP (to BUDGET_MIN_SPENDING). With a surplus (income over BUDGET_SURPLUS_RATIO x */ \
	/* the spending and twice the reserve in the treasury) the sliders go back toward 0.5 by the step. 0 = the sliders of the AI stay where they are */ \
	INT(BUDGET_ENABLED, budget_enabled, 1, 0, 1) \
	RATE(BUDGET_STEP, budget_step, Rate(5, 100), 0, 1) \
	INT(BUDGET_RESERVE_DAYS, budget_reserve_days, 30, 0, 3650) \
	RATE(BUDGET_MAX_TAX, budget_max_tax, Rate(80, 100), 0, 1) \
	RATE(BUDGET_MIN_SPENDING, budget_min_spending, Rate(20, 100), 0, 1) \
	RATE(BUDGET_SURPLUS_RATIO, budget_surplus_ratio, Rate(130, 100), 1, 100)
#define OV_POPULATION_DEFINES(INT, FIXED, RATE, DOUBLE) \
	RATE(GROWTH_BREAKEVEN_LIFE_NEEDS, growth_breakeven_life_needs, Rate(20, 100), 0, 1) \
	/* The life needs at and above which a pop grows by the whole base of the data (between the breakeven and this it grows with the life needs) */ \
	RATE(GROWTH_FULL_LIFE_NEEDS, growth_full_life_needs, Rate(80, 100), 0, 1) \
	/* The growth of a pop with no rule in ov_data/common/ov_population/growth.txt (the file has its own default; this one is used when there is none) */ \
	RATE(GROWTH_PER_YEAR, growth_per_year, Rate(7, 1000), 0, 10) \
	RATE(DECLINE_PER_YEAR, decline_per_year, Rate(60, 1000), 0, 10) \
	/* The medicine: the growth is multiplied by 1 + this x (1 - the casualties taken of the country); casualties_taken_mult 0.8 gives +10 % at 0.5 */ \
	RATE(MEDICINE_GROWTH_WEIGHT, medicine_growth_weight, Rate(50, 100), 0, 10) \
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
	RATE(SCORE_LIFE_NEEDS_WEIGHT, score_life_needs_weight, Rate(1, 2), 0, 1) \
	/* Surplus people: the unemployed of a pop type that can be unemployed. A pop is hungry when it has less than this share of its life */ \
	/* needs; the surplus of a pop that was hungry for HUNGER_DAYS_BEFORE_DEATH days of the month emigrates (EMIGRATION_SHARE of it, */ \
	/* the technologies raise it later) and the rest dies (SURPLUS_DEATH_SHARE of the rest, per month) */ \
	RATE(HUNGER_LIFE_NEEDS_THRESHOLD, hunger_life_needs_threshold, Rate(50, 100), 0, 1) \
	INT(HUNGER_DAYS_BEFORE_DEATH, hunger_days_before_death, 28, 1, 31) \
	RATE(EMIGRATION_SHARE, emigration_share, Rate(1, 100), 0, 1) \
	RATE(SURPLUS_DEATH_SHARE, surplus_death_share, Rate(80, 100), 0, 1) \
	/* The surplus moves inside the country to provinces with free jobs of the same pop type, and abroad only when the whole country */ \
	/* has no free jobs; at most this share of the free jobs of a destination is filled per month */ \
	RATE(IMMIGRATION_FILL_SHARE, immigration_fill_share, Rate(50, 100), 0, 1) \
	INT(STARVATION_MIN_SURPLUS, starvation_min_surplus, 1, 0, 1000000) \
	/* The migration to free jobs of the resource gathering operations (docs/POPULATION.md A3): every month JOB_MIGRATION_UNEMPLOYED_SHARE of the unemployed */ \
	/* of a pop go to a free job in the region or the next region of the country (the type of the pop changes when the job is of another type, farmer -> labourer); */ \
	/* the best place is the one where the RGO pays most for a worker. JOB_MIGRATION_PROFIT_SHARE of the employed move too when the place pays at least */ \
	/* JOB_MIGRATION_PROFIT_RATIO times more (fruit -> metal). 0 = off */ \
	INT(JOB_MIGRATION_ENABLED, job_migration_enabled, 1, 0, 1) \
	RATE(JOB_MIGRATION_UNEMPLOYED_SHARE, job_migration_unemployed_share, Rate(5, 100), 0, 1) \
	RATE(JOB_MIGRATION_PROFIT_SHARE, job_migration_profit_share, Rate(2, 1000), 0, 1) \
	FIXED(JOB_MIGRATION_PROFIT_RATIO, job_migration_profit_ratio, fixed_point_t::_1 + fixed_point_t::_0_50, 1, 100)
#define OV_POLITICS_DEFINES(INT, FIXED, RATE, DOUBLE) \
	DOUBLE(LEANING_ARISTOCRATS_REACTIONARY, leaning_aristocrats_reactionary, 0.45, 0, 1) \
	DOUBLE(LEANING_ARISTOCRATS_CONSERVATIVE, leaning_aristocrats_conservative, 0.45, 0, 1) \
	DOUBLE(LEANING_ARISTOCRATS_LIBERAL, leaning_aristocrats_liberal, 0.1, 0, 1) \
	DOUBLE(LEANING_CAPITALISTS_REACTIONARY, leaning_capitalists_reactionary, 0.1, 0, 1) \
	DOUBLE(LEANING_CAPITALISTS_CONSERVATIVE, leaning_capitalists_conservative, 0.35, 0, 1) \
	DOUBLE(LEANING_CAPITALISTS_LIBERAL, leaning_capitalists_liberal, 0.55, 0, 1) \
	DOUBLE(LEANING_BUREAUCRATS_REACTIONARY, leaning_bureaucrats_reactionary, 0.2, 0, 1) \
	DOUBLE(LEANING_BUREAUCRATS_CONSERVATIVE, leaning_bureaucrats_conservative, 0.4, 0, 1) \
	DOUBLE(LEANING_BUREAUCRATS_LIBERAL, leaning_bureaucrats_liberal, 0.4, 0, 1) \
	DOUBLE(LEANING_CLERGYMEN_REACTIONARY, leaning_clergymen_reactionary, 0.45, 0, 1) \
	DOUBLE(LEANING_CLERGYMEN_CONSERVATIVE, leaning_clergymen_conservative, 0.45, 0, 1) \
	DOUBLE(LEANING_CLERGYMEN_LIBERAL, leaning_clergymen_liberal, 0.1, 0, 1) \
	DOUBLE(LEANING_OFFICERS_REACTIONARY, leaning_officers_reactionary, 0.35, 0, 1) \
	DOUBLE(LEANING_OFFICERS_CONSERVATIVE, leaning_officers_conservative, 0.5, 0, 1) \
	DOUBLE(LEANING_OFFICERS_FASCIST, leaning_officers_fascist, 0.1, 0, 1) \
	DOUBLE(LEANING_OFFICERS_LIBERAL, leaning_officers_liberal, 0.05, 0, 1) \
	DOUBLE(LEANING_SOLDIERS_REACTIONARY, leaning_soldiers_reactionary, 0.3, 0, 1) \
	DOUBLE(LEANING_SOLDIERS_CONSERVATIVE, leaning_soldiers_conservative, 0.5, 0, 1) \
	DOUBLE(LEANING_SOLDIERS_LIBERAL, leaning_soldiers_liberal, 0.2, 0, 1) \
	DOUBLE(LEANING_FARMERS_REACTIONARY, leaning_farmers_reactionary, 0.35, 0, 1) \
	DOUBLE(LEANING_FARMERS_CONSERVATIVE, leaning_farmers_conservative, 0.5, 0, 1) \
	DOUBLE(LEANING_FARMERS_LIBERAL, leaning_farmers_liberal, 0.15, 0, 1) \
	DOUBLE(LEANING_LABOURERS_CONSERVATIVE, leaning_labourers_conservative, 0.25, 0, 1) \
	DOUBLE(LEANING_LABOURERS_LIBERAL, leaning_labourers_liberal, 0.45, 0, 1) \
	DOUBLE(LEANING_LABOURERS_SOCIALIST, leaning_labourers_socialist, 0.3, 0, 1) \
	DOUBLE(LEANING_CRAFTSMEN_CONSERVATIVE, leaning_craftsmen_conservative, 0.2, 0, 1) \
	DOUBLE(LEANING_CRAFTSMEN_LIBERAL, leaning_craftsmen_liberal, 0.5, 0, 1) \
	DOUBLE(LEANING_CRAFTSMEN_SOCIALIST, leaning_craftsmen_socialist, 0.3, 0, 1) \
	DOUBLE(LEANING_CLERKS_CONSERVATIVE, leaning_clerks_conservative, 0.3, 0, 1) \
	DOUBLE(LEANING_CLERKS_LIBERAL, leaning_clerks_liberal, 0.6, 0, 1) \
	DOUBLE(LEANING_CLERKS_SOCIALIST, leaning_clerks_socialist, 0.1, 0, 1) \
	DOUBLE(LEANING_SLAVES_CONSERVATIVE, leaning_slaves_conservative, 0.3, 0, 1) \
	DOUBLE(LEANING_SLAVES_LIBERAL, leaning_slaves_liberal, 0.3, 0, 1) \
	DOUBLE(LEANING_SLAVES_SOCIALIST, leaning_slaves_socialist, 0.4, 0, 1) \
	DOUBLE(LEANING_STRATA_RICH_REACTIONARY, leaning_strata_rich_reactionary, 0.3, 0, 1) \
	DOUBLE(LEANING_STRATA_RICH_CONSERVATIVE, leaning_strata_rich_conservative, 0.4, 0, 1) \
	DOUBLE(LEANING_STRATA_RICH_LIBERAL, leaning_strata_rich_liberal, 0.3, 0, 1) \
	DOUBLE(LEANING_STRATA_MIDDLE_CONSERVATIVE, leaning_strata_middle_conservative, 0.35, 0, 1) \
	DOUBLE(LEANING_STRATA_MIDDLE_LIBERAL, leaning_strata_middle_liberal, 0.55, 0, 1) \
	DOUBLE(LEANING_STRATA_MIDDLE_SOCIALIST, leaning_strata_middle_socialist, 0.1, 0, 1) \
	DOUBLE(LEANING_STRATA_OTHER_REACTIONARY, leaning_strata_other_reactionary, 0.2, 0, 1) \
	DOUBLE(LEANING_STRATA_OTHER_CONSERVATIVE, leaning_strata_other_conservative, 0.45, 0, 1) \
	DOUBLE(LEANING_STRATA_OTHER_LIBERAL, leaning_strata_other_liberal, 0.2, 0, 1) \
	DOUBLE(LEANING_STRATA_OTHER_SOCIALIST, leaning_strata_other_socialist, 0.15, 0, 1) \
	DOUBLE(POSITION_REACTIONARY, position_reactionary, 0.0, 0, 1) \
	DOUBLE(POSITION_CONSERVATIVE, position_conservative, 0.25, 0, 1) \
	DOUBLE(POSITION_FASCIST, position_fascist, 0.35, 0, 1) \
	DOUBLE(POSITION_LIBERAL, position_liberal, 0.6, 0, 1) \
	DOUBLE(POSITION_ANARCHO_LIBERAL, position_anarcho_liberal, 0.75, 0, 1) \
	DOUBLE(POSITION_SOCIALIST, position_socialist, 0.85, 0, 1) \
	DOUBLE(POSITION_COMMUNIST, position_communist, 1.0, 0, 1) \
	DOUBLE(POSITION_OTHER, position_other, 0.5, 0, 1) \
	DOUBLE(CLOSENESS_WIDTH, closeness_width, 0.3, 0.001, 10) \
	DOUBLE(IDEOLOGY_BASE_WEIGHT, ideology_base_weight, 0.01, 0, 10) \
	DOUBLE(REACTIONARY_CONSCIOUSNESS_PENALTY, reactionary_consciousness_penalty, 0.6, 0, 1) \
	DOUBLE(CONSERVATIVE_CONSCIOUSNESS_PENALTY, conservative_consciousness_penalty, 0.3, 0, 1) \
	DOUBLE(LIBERAL_CONSCIOUSNESS_BONUS, liberal_consciousness_bonus, 0.8, 0, 100) \
	DOUBLE(LIBERAL_LITERACY_BONUS, liberal_literacy_bonus, 0.5, 0, 100) \
	DOUBLE(SOCIALIST_BASE, socialist_base, 0.3, 0, 100) \
	DOUBLE(SOCIALIST_CONSCIOUSNESS_BONUS, socialist_consciousness_bonus, 1.5, 0, 100) \
	DOUBLE(SOCIALIST_MILITANCY_BONUS, socialist_militancy_bonus, 1.0, 0, 100) \
	DOUBLE(COMMUNIST_BASE, communist_base, 0.05, 0, 100) \
	DOUBLE(COMMUNIST_CONSCIOUSNESS_MILITANCY_BONUS, communist_consciousness_militancy_bonus, 1.0, 0, 100) \
	DOUBLE(ANARCHO_LIBERAL_BASE, anarcho_liberal_base, 0.1, 0, 100) \
	DOUBLE(ANARCHO_LIBERAL_MILITANCY_BONUS, anarcho_liberal_militancy_bonus, 1.2, 0, 100) \
	DOUBLE(FASCIST_BASE, fascist_base, 0.1, 0, 100) \
	DOUBLE(FASCIST_MILITANCY_BONUS, fascist_militancy_bonus, 0.8, 0, 100) \
	DOUBLE(UNREST_LIFE_NEEDS_THRESHOLD, unrest_life_needs_threshold, 0.5, 0, 1) \
	DOUBLE(UNREST_LIFE_NEEDS_WEIGHT, unrest_life_needs_weight, 2.0, 0, 100) \
	DOUBLE(UNREST_UNEMPLOYMENT_WEIGHT, unrest_unemployment_weight, 0.5, 0, 100) \
	DOUBLE(UNREST_WAR_EXHAUSTION_WEIGHT, unrest_war_exhaustion_weight, 0.3, 0, 100) \
	DOUBLE(MILITANCY_PRESSURE_OFFSET, militancy_pressure_offset, 0.25, 0, 100) \
	DOUBLE(MILITANCY_GROWTH, militancy_growth, 0.35, 0, 100) \
	DOUBLE(CONSCIOUSNESS_LITERACY_GROWTH, consciousness_literacy_growth, 0.04, 0, 100) \
	DOUBLE(CONSCIOUSNESS_DECAY, consciousness_decay, 0.01, 0, 100) \
	DOUBLE(UPRISING_END_MILITANCY_DROP, uprising_end_militancy_drop, 2.0, 0, 10) \
	INT(MILITANT_LEVEL, militant_level, 6, 0, 9) \
	INT(MIN_MILITANT_PEOPLE, min_militant_people, 30000, 0, 100000000) \
	INT(PEOPLE_PER_REBEL_REGIMENT, people_per_rebel_regiment, 12000, 1, 100000000) \
	INT(MAX_REBEL_REGIMENTS, max_rebel_regiments, 6, 1, 1000) \
	DOUBLE(MIN_MILITANT_SHARE, min_militant_share, 0.15, 0, 1) \
	INT(UPRISING_COOLDOWN_DAYS, uprising_cooldown_days, 365, 0, 36500) \
	INT(AI_SUPPRESSION_DAYS, ai_suppression_days, 90, 0, 36500)
#define OV_COLONISATION_DEFINES(INT, FIXED, RATE, DOUBLE) \
	INT(MAX_COLONIAL_RANK, max_colonial_rank, 16, 1, 1000) \
	INT(SEA_RANGE_BASE, sea_range_base, 3, 0, 1000) \
	INT(SEA_RANGE_PER_BASE_LEVEL, sea_range_per_base_level, 2, 0, 1000) \
	INT(MIN_COLONISATION_DAYS, min_colonisation_days, 30, 1, 36500)
#define OV_WAR_DEFINES(INT, FIXED, RATE, DOUBLE) \
	INT(TRUCE_DAYS, truce_days, 1825, 0, 36500) \
	INT(SIEGE_BASE_DAYS, siege_base_days, 60, 0, 36500) \
	INT(SIEGE_DAYS_PER_FORT_LEVEL, siege_days_per_fort_level, 100, 0, 36500) \
	INT(CAPITAL_PROVINCE_WEIGHT, capital_province_weight, 5, 1, 1000) \
	INT(WAR_SCORE_MAX, war_score_max, 100, 1, 10000) \
	INT(MAX_BATTLE_SCORE, max_battle_score, 25, 0, 10000) \
	INT(BATTLE_SCORE_REGIMENT_CAP, battle_score_regiment_cap, 20, 0, 1000) \
	INT(BATTLE_SCORE_REGIMENT_DIVISOR, battle_score_regiment_divisor, 4, 1, 1000) \
	INT(NAVAL_BATTLE_SCORE_SHIP_FACTOR, naval_battle_score_ship_factor, 2, 0, 1000) \
	INT(NAVAL_BATTLE_SCORE_SHIP_CAP, naval_battle_score_ship_cap, 12, 0, 1000) \
	INT(LEADER_POINT_DIVISOR, leader_point_divisor, 10, 1, 1000) \
	RATE(LEADER_MIN_MULTIPLIER, leader_min_multiplier, Rate(1, 4), 0, 1) \
	INT(LEADER_DEATH_ODDS, leader_death_odds, 30, 1, 1000000) \
	RATE(LEADER_VICTORY_PRESTIGE, leader_victory_prestige, Rate(1, 100), 0, 100) \
	RATE(LEADER_DEFEAT_PRESTIGE, leader_defeat_prestige, Rate(1, 100), 0, 100) \
	RATE(MIN_ORGANISATION_FACTOR, min_organisation_factor, Rate(3, 10), 0, 1) \
	INT(ROUT_STRENGTH_MULTIPLIER, rout_strength_multiplier, 3, 1, 1000) \
	RATE(ROUT_THRESHOLD, rout_threshold, Rate(1, 10), 0, 1) \
	RATE(SIEGE_SPEED_PER_REGIMENT, siege_speed_per_regiment, Rate(1, 8), 0, 100) \
	INT(SIEGE_SKILL_DIVISOR, siege_skill_divisor, 10, 1, 1000) \
	INT(SIEGE_SPEED_MAX, siege_speed_max, 4, 1, 1000) \
	RATE(ORGANISATION_RECOVERY, organisation_recovery, Rate(2, 100), 0, 1) \
	RATE(STRENGTH_RECOVERY, strength_recovery, Rate(5, 1000), 0, 1) \
	RATE(RECOVERY_MIN_SUPPLY, recovery_min_supply, Rate(1, 4), 0, 1) \
	RATE(STRENGTH_RECOVERY_MIN_SUPPLY, strength_recovery_min_supply, Rate(1, 2), 0, 1) \
	/* The manpower (docs/RECRUITMENT.md): the maximum is the pops of soldiers MANPOWER_BASE_BONUS bigger, at war MANPOWER_WAR_BONUS more of */ \
	/* the pops are added; an army over the maximum fights with the attack and */ \
	/* the defence times 1 - OVER_MANPOWER_PENALTY x the share over it, not below OVER_MANPOWER_MIN_MULT; the new men of the regiments come out of */ \
	/* the free manpower and cost REINFORCE_GOODS_SHARE of the build cost of the regiment for its share of the strength; the armies of the start */ \
	/* over the maximum start weaker, at the maximum (START_MANPOWER_FIT); an AI country over it at peace disbands its weakest regiments */ \
	/* The war exhaustion (docs/WAR_ASSUMPTIONS.md): every day at war a country gets WAR_EXHAUSTION_PER_LOSS_SHARE points for the share of its manpower */ \
	/* that died that day (times 1 + war_exhaustion of the technologies), WAR_EXHAUSTION_OCCUPATION_PER_DAY for the share of its population under */ \
	/* the enemy and WAR_EXHAUSTION_BLOCKADE_PER_DAY for the share of its ports blockaded (the total_occupation, total_blockaded of Vic2); every month */ \
	/* the war_exhaustion of the modifiers (at peace -1); at most max_war_exhaustion (100 at war, 50 at peace) */ \
	FIXED(WAR_EXHAUSTION_PER_LOSS_SHARE, war_exhaustion_per_loss_share, fixed_point_t(100), 0, 10000) \
	/* An AI ally joins a war it is called into while its war exhaustion is at most CALL_ALLY_MAX_EXHAUSTION (and it fights no other war); an AI country */ \
	/* that does not lead its side asks for a separate peace once its war exhaustion is SEPARATE_PEACE_AI_TIREDNESS of its maximum */ \
	FIXED(CALL_ALLY_MAX_EXHAUSTION, call_ally_max_exhaustion, fixed_point_t(25), 0, 1000) \
	/* The justification of a war: JUSTIFY_WAR_DAYS days against a country, then JUSTIFY_WAR_INFAMY infamy and a casus belli for CASUS_BELLI_DAYS days; */ \
	/* a war with it costs nothing (it is used up), a war without it NO_CB_WAR_INFAMY infamy and NO_CB_WAR_EXHAUSTION war exhaustion at once */ \
	INT(JUSTIFY_WAR_DAYS, justify_war_days, 120, 1, 3650) \
	FIXED(JUSTIFY_WAR_INFAMY, justify_war_infamy, fixed_point_t(2), 0, 100) \
	INT(CASUS_BELLI_DAYS, casus_belli_days, 365, 1, 36500) \
	FIXED(NO_CB_WAR_INFAMY, no_cb_war_infamy, fixed_point_t(5), 0, 100) \
	FIXED(NO_CB_WAR_EXHAUSTION, no_cb_war_exhaustion, fixed_point_t(10), 0, 100) \
	RATE(SEPARATE_PEACE_AI_TIREDNESS, separate_peace_ai_tiredness, Rate(50, 100), 0, 1) \
	FIXED(WAR_EXHAUSTION_OCCUPATION_PER_DAY, war_exhaustion_occupation_per_day, fixed_point_t(5) / 100, 0, 100) \
	FIXED(WAR_EXHAUSTION_BLOCKADE_PER_DAY, war_exhaustion_blockade_per_day, fixed_point_t(1) / 2, 0, 100) \
	RATE(MANPOWER_BASE_BONUS, manpower_base_bonus, Rate(10, 100), 0, 10) \
	RATE(MANPOWER_WAR_BONUS, manpower_war_bonus, Rate(10, 100), 0, 10) \
	RATE(OVER_MANPOWER_PENALTY, over_manpower_penalty, Rate(1, 1), 0, 100) \
	RATE(OVER_MANPOWER_MIN_MULT, over_manpower_min_mult, Rate(25, 100), 0, 1) \
	/* An army at most this share over the maximum has no penalty (and no message): the pops of soldiers change a little from month to month */ \
	RATE(OVER_MANPOWER_TOLERANCE, over_manpower_tolerance, Rate(2, 100), 0, 1) \
	RATE(REINFORCE_GOODS_SHARE, reinforce_goods_share, Rate(1, 1), 0, 100) \
	INT(START_MANPOWER_FIT, start_manpower_fit, 1, 0, 1) \
	INT(AI_DISBAND_OVER_MANPOWER, ai_disband_over_manpower, 1, 0, 1) \
	/* The mobilisation (docs/RECRUITMENT.md, "Мобилизация"): the second pool of men is the poor pops that are not soldiers nor slaves (farmers, */ \
	/* labourers, craftsmen) of the allowed cultures in the own, not occupied provinces times MOBILISATION_POP_SHARE + the mobilisation_size of */ \
	/* the modifiers (the national values of Vic2 give 0.02-0.06), times 1 + mobilisation_size_pct, at least MIN_MOBILIZE_LIMIT (Vic2) */ \
	/* regiments. A province raises its regiments in MOBILISATION_DAYS x MOBILISATION_REFERENCE_SPEED / speed days, the speed is */ \
	/* MOBILIZATION_SPEED_BASE x (1 + MOBILIZATION_SPEED_RAILS_MULT x the railroad level 0..1) (Vic2) x (1 + mobilisation_speed_pct) x */ \
	/* mobilisation_speed_mult. While the men are away the RGOs and the factories of the country make 1 - impact x the share of the pool */ \
	/* base that is mobilised (impact = MOBILISATION_ECONOMY_IMPACT_BASE + mobilisation_economy_impact of the modifiers), not below */ \
	/* MOBILISATION_OUTPUT_MIN_MULT. A country that was at war while mobilised demobilises DEMOBILISE_DAYS after its last war ends */ \
	RATE(MOBILISATION_POP_SHARE, mobilisation_pop_share, Rate(3, 100), 0, 1) \
	/* The land that is not national (not a core of the country: colonies, conquests) gives MOBILISATION_NON_NATIONAL_FACTOR of the share; a region */ \
	/* (a state) gives at most MOBILISATION_REGION_MAX of its population, of its land that is not national MOBILISATION_REGION_MAX_NON_NATIONAL (the */ \
	/* reforms and the technologies will change them; decision of the owner 2026-11-07) */ \
	RATE(MOBILISATION_NON_NATIONAL_FACTOR, mobilisation_non_national_factor, Rate(1, 100), 0, 1) \
	RATE(MOBILISATION_REGION_MAX, mobilisation_region_max, Rate(25, 100), 0, 1) \
	RATE(MOBILISATION_REGION_MAX_NON_NATIONAL, mobilisation_region_max_non_national, Rate(25, 10000), 0, 1) \
	INT(MOBILISATION_DAYS, mobilisation_days, 30, 1, 3650) \
	RATE(MOBILISATION_REFERENCE_SPEED, mobilisation_reference_speed, Rate(12, 100), 0.001, 1000) \
	RATE(MOBILISATION_ECONOMY_IMPACT_BASE, mobilisation_economy_impact_base, Rate(0, 1), 0, 100) \
	RATE(MOBILISATION_OUTPUT_MIN_MULT, mobilisation_output_min_mult, Rate(50, 100), 0, 1) \
	INT(DEMOBILISE_DAYS, demobilise_days, 30, 0, 3650) \
	/* The AI mobilises at war when its regiments (not mobilised) are fewer than AI_MOBILISE_RATIO x the regiments of its enemies; 0 turns it off */ \
	INT(AI_MOBILISE, ai_mobilise, 1, 0, 1) \
	RATE(AI_MOBILISE_RATIO, ai_mobilise_ratio, Rate(1, 1), 0, 100) \
	/* At most RECRUIT_MAX_REGIMENTS_PER_MONTH regiments a month start their recruitment in a country (0: no limit); the orders over it wait */ \
	/* in the queue and start in the next months by their priority (higher first, then the older) */ \
	INT(RECRUIT_MAX_REGIMENTS_PER_MONTH, recruit_max_regiments_per_month, 0, 0, 100000) \
	RATE(NAVAL_STRENGTH_DAMAGE, naval_strength_damage, Rate(2, 100), 0, 100) \
	RATE(NAVAL_ORGANISATION_DAMAGE, naval_organisation_damage, Rate(8, 100), 0, 100) \
	INT(NAVAL_EVASION_DIVISOR, naval_evasion_divisor, 10, 1, 1000) \
	INT(NAVAL_MAX_DAMAGE_RATIO, naval_max_damage_ratio, 6, 1, 1000) \
\
	INT(FRONT_SPREAD_MAX_SPLITS_PER_DAY, front_spread_max_splits_per_day, 4, 0, 100) \
	/* The zone of an offensive (docs/WAR_ASSUMPTIONS.md, section 6): the targets and the enemy provinces on the shortest ways (in provinces) from the */ \
	/* front to them, with this many steps more (the width of the corridor of the arrow) */ \
	INT(PLAN_ZONE_SLACK, plan_zone_slack, 1, 0, 20) \
	/* The divisions of a field marshal along the line of his order (P4): every province of the line gets a share of them by its weight, */ \
	/* FRONT_MIN_WEIGHT plus the enemy divisions next to it; a placed division moves to even the line out when its province has this many */ \
	/* divisions more than its share (the medium integrity of the front; the loose one moves at 1, the firm one never); the armies of an order */ \
	/* are split into divisions of their own, at most this many splits a day for every order */ \
	INT(FRONT_MIN_WEIGHT, front_min_weight, 1, 1, 100) \
	INT(FRONT_REBALANCE_THRESHOLD, front_rebalance_threshold, 2, 1, 100) \
	INT(FRONT_GROUP_SPLITS_PER_DAY, front_group_splits_per_day, 64, 0, 1000) \
	/* A line of defence is a chain of neighbouring provinces: a gap is filled by the shortest way over our land, at most this many provinces */ \
	INT(PLAN_LINE_MAX_STEPS, plan_line_max_steps, 40, 1, 500) \
	/* The chance of the victory of a battle (the estimate): q = the days the attackers last / the days until the defence leaves (its organisation or */ \
	/* the rule of R), the chance = q^BATTLE_WIN_STEEPNESS / (q^BATTLE_WIN_STEEPNESS + 1): 50 % at q = 1; with 3: 65 % at q = 1.23, 80 % at q = 1.59 */ \
	INT(BATTLE_WIN_STEEPNESS, battle_win_steepness, 3, 1, 8) \
	/* The executor of an offensive (P5): a target next to the line is attacked by the free divisions next to it (the support from several */ \
	/* provinces) when the chance of the victory is at least PLAN_WIN_CAREFUL (the careful mode), PLAN_WIN_BALANCED (the balanced one) or */ \
	/* PLAN_WIN_AGGRESSIVE (the aggressive one). PLAN_MIN_HOLD divisions stay in every province of the front (not in the aggressive mode). */ \
	/* The order of the targets: the chance times PLAN_SCORE_CHANCE_WEIGHT plus PLAN_TARGET_WEIGHT / (1 + the steps to the line of the plan) */ \
	RATE(PLAN_WIN_CAREFUL, plan_win_careful, Rate(80, 100), 0, 1) \
	RATE(PLAN_WIN_BALANCED, plan_win_balanced, Rate(65, 100), 0, 1) \
	RATE(PLAN_WIN_AGGRESSIVE, plan_win_aggressive, Rate(50, 100), 0, 1) \
	INT(PLAN_MIN_HOLD, plan_min_hold, 1, 0, 100) \
	/* The breakthrough (P7): this share of the divisions of the field marshal (the strongest by their attack, at least one) goes along its way of */ \
	/* provinces into the enemy land with the threshold of the aggressive mode, without the minimum to hold */ \
	RATE(PLAN_BREAKTHROUGH_SHARE, plan_breakthrough_share, Rate(1, 3), 0, 1) \
	/* The bonus of the planning of a field marshal (P6): while he has an order and does not execute it the bonus grows by PLANNING_GAIN_PER_DAY a */ \
	/* day (times 1 + the modifier planning_speed_mult) up to PLANNING_MAX (plus the modifier planning_max_add); while he executes it falls by */ \
	/* PLANNING_DECAY_PER_DAY a day. It multiplies the attack and the breakthrough of his divisions in a battle (1 + the bonus), not of a division */ \
	/* that moves by a manual order */ \
	RATE(PLANNING_GAIN_PER_DAY, planning_gain_per_day, Rate(1, 100), 0, 1) \
	RATE(PLANNING_MAX, planning_max, Rate(30, 100), 0, 10) \
	RATE(PLANNING_DECAY_PER_DAY, planning_decay_per_day, Rate(2, 100), 0, 1) \
	FIXED(PLAN_SCORE_CHANCE_WEIGHT, plan_score_chance_weight, fixed_point_t(10), 0, 1000) \
	FIXED(PLAN_TARGET_WEIGHT, plan_target_weight, fixed_point_t(2), 0, 1000) \
	/* The encirclements (P8): an army of a country at war is cut off when no way over the land of the country and its friends leads to it from */ \
	/* a source of the supply (the capital, a port with a naval base that the country holds and no enemy blockades). It gets no supply and no */ \
	/* new men, loses ENCIRCLED_ORG_LOSS_PER_DAY of its organisation a day, and gives itself up after ENCIRCLED_SURRENDER_DAYS days without it */ \
	RATE(ENCIRCLED_ORG_LOSS_PER_DAY, encircled_org_loss_per_day, Rate(5, 100), 0, 1) \
	INT(ENCIRCLED_SURRENDER_DAYS, encircled_surrender_days, 7, 1, 365) \
	/* The executor in the careful and the balanced mode does not take a target whose land neighbours are held by the enemy for more than this */ \
	/* share (the flank it would open); a target that closes a pocket (an enemy province next to it has no other way to its land) gets this more */ \
	RATE(FLANK_EXPOSURE_MAX, flank_exposure_max, Rate(60, 100), 0, 1) \
	/* The fallback (P9): a division of a field marshal on his line next to the enemy with less than FALLBACK_ORG_THRESHOLD of its organisation */ \
	/* falls back to a province of the country behind the line (no enemy next to it); off the line it recovers until FALLBACK_RETURN_ORG, then it */ \
	/* goes back to the line */ \
	RATE(FALLBACK_ORG_THRESHOLD, fallback_org_threshold, Rate(25, 100), 0, 1) \
	RATE(FALLBACK_RETURN_ORG, fallback_return_org, Rate(60, 100), 0, 1) \
	/* The naval invasion (P10): the divisions gather in the port; the plan is executed after NAVAL_PLAN_PREP_DAYS days of the preparation: they board */ \
	/* the fleets in the port, the fleets sail to the sea next to the target and land them. The organisation the landing takes is times */ \
	/* (1 - the bonus of the planning x NAVAL_PLAN_PENALTY_REDUCTION) (a landing by hand: the whole of it) */ \
	INT(NAVAL_PLAN_PREP_DAYS, naval_plan_prep_days, 10, 0, 365) \
	/* The AI countries with mechanic:command use the field marshals (P11): 1 makes them, a group, a front and an offensive for every enemy */ \
	/* they border; 0 (the default) keeps their earlier plan (docs/PENDING_PLANS.md) */ \
	INT(PLAN_AI_ENABLED, plan_ai_enabled, 0, 0, 1) \
	FIXED(NAVAL_PLAN_PENALTY_REDUCTION, naval_plan_penalty_reduction, fixed_point_t(2), 0, 100) \
	FIXED(PLAN_POCKET_WEIGHT, plan_pocket_weight, fixed_point_t(3), 0, 1000) \
	/* The experience of the leaders (docs/WAR_ASSUMPTIONS.md, section 7, military/LeaderSkills.*). A general learns in battles: every day of a */ \
	/* battle of his army GENERAL_XP_PER_BATTLE_DAY of the attack (his army attacks) or of the defence (it defends) and GENERAL_TERRAIN_XP_SHARE of */ \
	/* that of the terrain of the province. Level n of a skill needs GENERAL_XP_PER_LEVEL x (1 + 2 + ... + n), at most GENERAL_SKILL_MAX_LEVEL; a */ \
	/* level of the attack or the defence gives GENERAL_SKILL_STEP more of it to his divisions, one of a terrain makes the penalty of the terrain */ \
	/* GENERAL_SKILL_STEP smaller and its bonus GENERAL_SKILL_STEP bigger (a share of them). A field marshal learns every day of a war of his */ \
	/* country MARSHAL_XP_PER_DAY of the supply (his group has armies) and of the planning (it has an order); level n needs MARSHAL_XP_PER_LEVEL */ \
	/* x (1 + ... + n), at most MARSHAL_SKILL_MAX_LEVEL. A level of the supply: the armies of the group need MARSHAL_SKILL_STEP less and get */ \
	/* MARSHAL_SKILL_STEP more of the supply limit of their province; of the planning: the bonus grows MARSHAL_SKILL_STEP faster, falls */ \
	/* MARSHAL_SKILL_STEP slower and is worth MARSHAL_SKILL_STEP more. The role decides: a general that is a marshal learns and uses only the */ \
	/* skills of a marshal. A new level n gives the country n x GENERAL_LEVEL_PRESTIGE (a general) or n x MARSHAL_LEVEL_PRESTIGE of prestige */ \
	FIXED(GENERAL_XP_PER_BATTLE_DAY, general_xp_per_battle_day, fixed_point_t::_1, 0, 1000) \
	RATE(GENERAL_TERRAIN_XP_SHARE, general_terrain_xp_share, Rate(50, 100), 0, 10) \
	INT(GENERAL_XP_PER_LEVEL, general_xp_per_level, 10, 1, 100000) \
	INT(GENERAL_SKILL_MAX_LEVEL, general_skill_max_level, 5, 0, 20) \
	RATE(GENERAL_SKILL_STEP, general_skill_step, Rate(5, 100), 0, 1) \
	FIXED(MARSHAL_XP_PER_DAY, marshal_xp_per_day, fixed_point_t::_1, 0, 1000) \
	INT(MARSHAL_XP_PER_LEVEL, marshal_xp_per_level, 30, 1, 100000) \
	INT(MARSHAL_SKILL_MAX_LEVEL, marshal_skill_max_level, 3, 0, 20) \
	RATE(MARSHAL_SKILL_STEP, marshal_skill_step, Rate(5, 100), 0, 1) \
	FIXED(GENERAL_LEVEL_PRESTIGE, general_level_prestige, fixed_point_t::_1, 0, 1000) \
	FIXED(MARSHAL_LEVEL_PRESTIGE, marshal_level_prestige, fixed_point_t(5), 0, 1000) \
	/* The limits of the division templates (docs/PLAN_ARMY_SUPPLY_FRONTS.md, 4b): the ranks (columns) and the depth (rows) of the grid and the */ \
	/* slots of the support companies of a rank. A country starts with the default and gets more from the effects max_columns, max_rows and */ \
	/* max_company_slots of its modifiers (technologies, unlocks); never more than the maximum */ \
	INT(TEMPLATE_RANKS_DEFAULT, template_ranks_default, 3, 1, 12) \
	INT(TEMPLATE_RANKS_MAX, template_ranks_max, 6, 1, 12) \
	INT(TEMPLATE_DEPTH_DEFAULT, template_depth_default, 2, 1, 8) \
	INT(TEMPLATE_DEPTH_MAX, template_depth_max, 4, 1, 8) \
	INT(TEMPLATE_COMPANY_SLOTS_DEFAULT, template_company_slots_default, 1, 0, 8) \
	INT(TEMPLATE_COMPANY_SLOTS_MAX, template_company_slots_max, 4, 0, 8) \
	/* 1: the provisional unlocks of the year (ov_data/common/ov_equipment/provisional_unlocks.txt) give the weapons and the limits of the */ \
	/* templates by the year of the game to the AI countries (the player researches himself; not on the first day, the player is not known yet); */ \
	/* 0 when the technologies of the army give them to everyone (TEMPORARY, docs/PENDING_PLANS.md) */ \
	INT(PROVISIONAL_UNLOCKS_ENABLED, provisional_unlocks_enabled, 1, 0, 1) \
	/* What a template is worth (military/DivisionStats.cpp): the men of a strength point of a regiment, the kit and the men weigh this many */ \
	/* kilograms per strength point, the occupation speed 1 is the base (technologies add to it, from this percent it is instant) */ \
	INT(TEMPLATE_MEN_PER_STRENGTH, template_men_per_strength, 1000, 1, 100000) \
	INT(TEMPLATE_KIT_WEIGHT_PER_STRENGTH, template_kit_weight_per_strength, 100000, 0, 10000000) \
	/* The occupation of a province (military/BattleFormulas.cpp): the power of a division is the sum of the occupation weights of its regiments over */ \
	/* OCCUPATION_REFERENCE_REGIMENTS (6 = 3 ranks of 2); the days are the base days over (power x (1 + the sum of occupation_speed_pct) x the product of */ \
	/* occupation_speed_mult); OCCUPATION_BASE_DAYS is the base of a plain province in the dump of the templates (the siege has its own days) */ \
	INT(OCCUPATION_REFERENCE_REGIMENTS, occupation_reference_regiments, 6, 1, 1000) \
	INT(OCCUPATION_BASE_DAYS, occupation_base_days, 10, 1, 3650) \
	/* The armies (their generals) that can be attached to the field marshal of a group: the base, the modifier attached_generals_add of the */ \
	/* technologies raises it, but never above the limit */ \
	INT(MARSHAL_MAX_ATTACHED, marshal_max_attached, 12, 1, 64) \
	INT(MARSHAL_MAX_ATTACHED_LIMIT, marshal_max_attached_limit, 48, 1, 64) \
	/* How many theatres of the field marshals a country can have (the main one included) */ \
	INT(THEATRES_MAX, theatres_max, 8, 1, 32) \
	/* 1: the transports carry any number of regiments (for now); 0: REGIMENTS_PER_TRANSPORT of every transport ship */ \
	INT(TRANSPORT_UNLIMITED, transport_unlimited, 1, 0, 1) \
	/* The weapons of the regiments (military/RegimentEquipment in CountryInstance.cpp): how many regiments a day get a weapon of a better */ \
	/* generation out of the national stockpile (the government buys the goods for them), and the share of the attack, the defence and the */ \
	/* breakthrough a unit loses for every generation below the lowest weapon its type may have, when the country has none of them */ \
	INT(EQUIPMENT_UPGRADES_PER_DAY, equipment_upgrades_per_day, 1, 0, 1000) \
	RATE(EQUIPMENT_BELOW_MIN_PENALTY, equipment_below_min_penalty, Rate(25, 100), 0, 1) \
	/* A level of the fortification adds this share to the defence */ \
	RATE(FORTIFICATION_PER_LEVEL, fortification_per_level, Rate(5, 100), 0, 10) \
	/* The density of a division (military/BattleFormulas.cpp): density = DENSITY_DEPTH_FACTOR x depth / width (a width twice the depth is 1.0); x = |1 - density|; */ \
	/* the penalty is DENSITY_LINEAR x x + DENSITY_QUADRATIC x x^2 (the rule of the owner is x^2 + x). Below 1 the defence and the breakthrough fall by the penalty; */ \
	/* above 1 the casualties and the breakthrough grow by it and the attack by DENSITY_ATTACK_SHARE of it */ \
	FIXED(DENSITY_DEPTH_FACTOR, density_depth_factor, fixed_point_t(2), 0.01, 100) \
	FIXED(DENSITY_LINEAR, density_linear, fixed_point_t::_1, 0, 10) \
	FIXED(DENSITY_QUADRATIC, density_quadratic, fixed_point_t::_1, 0, 10) \
	RATE(DENSITY_ATTACK_SHARE, density_attack_share, Rate(1, 2), 0, 1) \
	/* The battle of divisions (military/BattleFormulas.cpp battle_hit, docs/WAR_ASSUMPTIONS.md, section 6): damage = attack x conditions x density x */ \
	/* (1 - BATTLE_BLOCK_SHARE x min(1, protection / attack)); the men lost = damage x BATTLE_STRENGTH_PER_HIT (strength points) x casualties_taken_mult, */ \
	/* the organisation = damage x BATTLE_ORGANISATION_PER_HIT (points of the division: every regiment in proportion to its maximum). The width of a battle is BATTLE_WIDTH_BASE ranks (15 ranks of */ \
	/* 2 rows are the 30 regiments of BASE_COMBAT_WIDTH of Vic2) times 1 + the combat_width of the terrain; the other divisions wait in the reserve. */ \
	/* The defence of the terrain of Vic2 (0, 1, 2) adds TERRAIN_DEFENCE_PER_POINT each to the protection of the defender. The border of two provinces */ \
	/* is a river when at least RIVER_BORDER_MIN_SHARE of it runs along a river (the attacker coming over it has the river_crossing of */ \
	/* ov_data/common/ov_war/battle_conditions.txt) */ \
	RATE(BATTLE_BLOCK_SHARE, battle_block_share, Rate(75, 100), 0, 1) \
	FIXED(BATTLE_STRENGTH_PER_HIT, battle_strength_per_hit, fixed_point_t(25) / 1000, 0, 100) \
	FIXED(BATTLE_ORGANISATION_PER_HIT, battle_organisation_per_hit, fixed_point_t(6) / 10, 0, 1000) \
	INT(BATTLE_WIDTH_BASE, battle_width_base, 15, 1, 120) \
	RATE(TERRAIN_DEFENCE_PER_POINT, terrain_defence_per_point, Rate(10, 100), 0, 10) \
	RATE(RIVER_BORDER_MIN_SHARE, river_border_min_share, Rate(25, 100), 0, 1) \
	/* The attacker loses more (it walks into the fire): its losses of men and of organisation are times these */ \
	RATE(BATTLE_ATTACKER_STRENGTH_LOSS_MULT, battle_attacker_strength_loss_mult, Rate(5, 2), 0, 100) \
	RATE(BATTLE_ATTACKER_ORGANISATION_LOSS_MULT, battle_attacker_organisation_loss_mult, Rate(115, 100), 0, 100) \
	/* An attack from several provinces at once: every direction after the first gives the attackers BATTLE_FLANK_WIDTH more of the width and */ \
	/* BATTLE_FLANK_ATTACK more of the damage, up to BATTLE_FLANK_MAX_DIRECTIONS directions */ \
	RATE(BATTLE_FLANK_WIDTH, battle_flank_width, Rate(1, 3), 0, 10) \
	RATE(BATTLE_FLANK_ATTACK, battle_flank_attack, Rate(5, 100), 0, 10) \
	INT(BATTLE_FLANK_MAX_DIRECTIONS, battle_flank_max_directions, 3, 1, 10) \
	/* A division of the front with less than BATTLE_ROTATE_ORGANISATION of its organisation goes to the reserve when the reserve has a division */ \
	/* with at least BATTLE_ROTATE_RESERVE_ORGANISATION of it (that one takes its place) */ \
	RATE(BATTLE_ROTATE_ORGANISATION, battle_rotate_organisation, Rate(30, 100), 0, 1) \
	RATE(BATTLE_ROTATE_RESERVE_ORGANISATION, battle_rotate_reserve_organisation, Rate(60, 100), 0, 1) \
	/* The breakthrough (R = the breakthrough of the attackers in the battle / the protection of the defenders): R of BREAK_RATIO_DAY in one day, or */ \
	/* R of BREAK_RATIO_WEEK for BREAK_WEEK_DAYS days in a row, makes the defence retreat; a division with no organisation left retreats by itself */ \
	FIXED(BREAK_RATIO_DAY, break_ratio_day, fixed_point_t(3), 0, 1000) \
	FIXED(BREAK_RATIO_WEEK, break_ratio_week, fixed_point_t(2), 0, 1000) \
	INT(BREAK_WEEK_DAYS, break_week_days, 7, 1, 365) \
	RATE(ATTRITION_BASE, attrition_base, Rate(2, 1000), 0, 1) \
	RATE(ATTRITION_NO_SUPPLY, attrition_no_supply, Rate(3, 1000), 0, 1) \
	RATE(ATTRITION_FLOOR_STRENGTH, attrition_floor_strength, Rate(1, 5), 0, 1) \
	RATE(ATTRITION_FRIENDLY_MULTIPLIER, attrition_friendly_multiplier, Rate(1, 2), 0, 1) \
	/* The attrition of an overloaded province (at war, or at peace on the land of others): ATTRITION_OVERLOAD a day for every share of the load over the supply limit of */ \
	/* the province (the low limit of the mountains, the marshes, the jungle is their attrition), not more than ATTRITION_OVERLOAD_MAX a day */ \
	RATE(ATTRITION_OVERLOAD, attrition_overload, Rate(5, 1000), 0, 1) \
	RATE(ATTRITION_OVERLOAD_MAX, attrition_overload_max, Rate(15, 1000), 0, 1) \
	INT(BLOCKADE_SUPPLY_DIVISOR, blockade_supply_divisor, 2, 1, 1000) \
	INT(MOVEMENT_SPEED_SCALE, movement_speed_scale, 3, 1, 1000) \
	INT(PORT_STEP_DISTANCE, port_step_distance, 10, 1, 10000) \
	INT(REGIMENTS_PER_TRANSPORT, regiments_per_transport, 4, 0, 1000) \
	/* The way by sea of an army (P4d, docs/WAR_ASSUMPTIONS.md, section 6): an army with no way over the land goes to the nearest port of its */ \
	/* country, sails (SEA_TRANSFER_SPEED x MOVEMENT_SPEED_SCALE of the distance a day, past no navy of an enemy at war) to the port of its */ \
	/* country nearest to the destination and goes on over the land; at sea it does not fight. 0 turns it off */ \
	INT(SEA_TRANSFER_ENABLED, sea_transfer_enabled, 1, 0, 1) \
	FIXED(SEA_TRANSFER_SPEED, sea_transfer_speed, fixed_point_t(8), 0.1, 1000) \
	INT(LANDING_ORGANISATION_DIVISOR, landing_organisation_divisor, 2, 1, 1000)
#define OV_PEACE_DEFINES(INT, FIXED, RATE, DOUBLE) \
	INT(OFFER_VALID_DAYS, offer_valid_days, 60, 1, 36500) \
	INT(DECLINE_COOLDOWN_DAYS, decline_cooldown_days, 30, 0, 36500) \
	INT(REPARATIONS_COST, reparations_cost, 10, 0, 10000) \
	INT(HUMILIATE_COST, humiliate_cost, 8, 0, 10000) \
	RATE(REPARATIONS_SHARE, reparations_share, Rate(25, 100), 0, 1) \
	INT(HUMILIATE_PRESTIGE, humiliate_prestige, 5, 0, 10000) \
	INT(WAR_GOAL_COST_DIVISOR, war_goal_cost_divisor, 2, 1, 1000) \
	INT(WHITE_PEACE_SCORE_GAP, white_peace_score_gap, 15, 0, 10000) \
	INT(WHITE_PEACE_WAR_DAYS, white_peace_war_days, 1095, 0, 36500) \
	RATE(WHITE_PEACE_TIREDNESS, white_peace_tiredness, Rate(1, 2), 0, 1) \
	INT(SURRENDER_SCORE, surrender_score, 40, 0, 10000) \
	INT(CAPITAL_LOST_SCORE, capital_lost_score, 25, 0, 10000) \
	INT(LONG_WAR_DAYS, long_war_days, 1825, 0, 36500) \
	INT(LONG_WAR_SCORE, long_war_score, 20, 0, 10000) \
	INT(CONCEDE_COST_FACTOR, concede_cost_factor, 2, 1, 1000) \
	INT(CONCEDE_SCORE_MARGIN, concede_score_margin, 5, 0, 10000) \
	RATE(CONCEDE_TIREDNESS, concede_tiredness, Rate(7, 10), 0, 1) \
	INT(DEMAND_OTHER_SCORE_DIVISOR, demand_other_score_divisor, 2, 1, 1000) \
	INT(DEMAND_STRONG_SCORE, demand_strong_score, 40, 0, 10000) \
	INT(DEMAND_STRONG_BONUS, demand_strong_bonus, 10, 0, 10000) \
	INT(DEMAND_TIREDNESS_BONUS, demand_tiredness_bonus, 20, 0, 10000) \
	INT(DEMAND_YEARS_CAP, demand_years_cap, 5, 0, 1000) \
	INT(DEMAND_YEAR_BONUS, demand_year_bonus, 4, 0, 10000) \
	INT(DEMAND_CAPITAL_BONUS, demand_capital_bonus, 15, 0, 10000) \
	INT(AI_PEACE_DAY_OF_MONTH, ai_peace_day_of_month, 10, 1, 28) \
	INT(AI_LEAD_SCORE, ai_lead_score, 30, 0, 10000) \
	INT(AI_LEAD_MARGIN, ai_lead_margin, 20, 0, 10000) \
	INT(AI_STALEMATE_DAYS, ai_stalemate_days, 540, 0, 36500) \
	RATE(AI_TERMS_SCORE_SHARE, ai_terms_score_share, Rate(7, 10), 0, 1) \
	INT(AI_RETRY_DAYS, ai_retry_days, 60, 0, 36500) \
	INT(AI_PLAYER_OFFER_EXTRA_DAYS, ai_player_offer_extra_days, 30, 0, 36500)
#define OV_RESEARCH_DEFINES(INT, FIXED, RATE, DOUBLE) \
	RATE(INVENTION_BASE_CHANCE_PER_MONTH, invention_base_chance_per_month, Rate(3, 100), 0, 1) \
	RATE(INVENTION_LITERACY_CHANCE, invention_literacy_chance, Rate(6, 100), 0, 1) \
	/* The technologies of the army (ov_data/common/ov_technology): 1: only the civilised countries research them. 0 (the default): every country researches */ \
	/* them itself, the civilised start with the technologies marked start = civilised, the uncivilised with none (they begin from the roots of the tree) */ \
	INT(ARMY_RESEARCH_CIVILISED_ONLY, army_research_civilised_only, 0, 0, 1) \
	/* 1: the AI countries start the available technology with the lowest year_hint when their slot of a branch is free. 0 (the default): the AI does not */ \
	/* research by these rules; the weapons and the limits of the templates come to the AI countries by the year (ov_war PROVISIONAL_UNLOCKS_ENABLED, */ \
	/* TEMPORARY until the AI researches by new rules, docs/PENDING_PLANS.md). The player researches himself */ \
	INT(ARMY_RESEARCH_AI_ENABLED, army_research_ai_enabled, 0, 0, 1) \
	/* 1 (for now): the technologies and the inventions of Victoria 2 work as in the base game. 0: the countries have none of them from the start, do not */ \
	/* research and do not invent (the definitions stay loaded, the history of the countries names them), the research of OpenVic (the technologies of */ \
	/* ov_data/common/ov_technology) takes their place. With 0 the economy loses their effects (the farms and mines +50 % and +30 %, tax efficiency, */ \
	/* factories, the supply...), see docs/PENDING_PLANS.md */ \
	INT(VIC2_TECHNOLOGIES_ENABLED, vic2_technologies_enabled, 1, 0, 1)
#define OV_ECONOMY_DEFINES(INT, FIXED, RATE, DOUBLE) \
	INT(MAX_FACTORIES_PER_STATE, max_factories_per_state, 12, 1, 64) \
	INT(FACTORY_VALUE_PAYBACK_YEARS, factory_value_payback_years, 6, 1, 1000) \
	INT(FACTORY_REPLACEMENT_VALUE_DIVISOR, factory_replacement_value_divisor, 4, 1, 1000) \
	INT(DEFAULT_CONSTRUCTION_DAYS, default_construction_days, 180, 1, 36500) \
	RATE(STARTING_BUDGET_SHARE, starting_budget_share, Rate(5, 100), 0, 10) \
	RATE(CAPITALIST_SAVINGS_SHARE, capitalist_savings_share, Rate(3, 10), 0, 1) \
	INT(STOCKPILE_TARGET_DAYS, stockpile_target_days, 2, 0, 365) \
	FIXED(EXPANSION_FUND_SHARE, expansion_fund_share, fixed_point_t::_0_10 * 2, 0, 1) \
	RATE(EXPANSION_MIN_EMPLOYMENT, expansion_min_employment, Rate(90, 100), 0, 1) \
	INT(WORKING_CAPITAL_DAYS, working_capital_days, 5, 0, 365) \
	INT(MIN_WORKING_CAPITAL, min_working_capital, 20, 0, 100000000) \
	INT(DIVIDEND_EXCESS_DIVISOR, dividend_excess_divisor, 10, 1, 1000) \
	INT(AVERAGE_PROFIT_DAYS, average_profit_days, 30, 1, 3650) \
	INT(DAYS_OF_LOSS_BEFORE_CLOSING, days_of_loss_before_closing, 90, 1, 36500) \
	FIXED(WAGE_SHARE, wage_share, fixed_point_t::_0_50, 0, 1) \
	INT(EFFICIENCY_GAIN_EPSILONS, efficiency_gain_epsilons, 65, 0, 1000000) \
	INT(EFFICIENCY_LOSS_EPSILONS, efficiency_loss_epsilons, 130, 0, 1000000) \
	FIXED(EFFICIENCY_MAX, efficiency_max, fixed_point_t::_1 + fixed_point_t::_0_50, 0.1, 100) \
	FIXED(EFFICIENCY_MIN, efficiency_min, fixed_point_t::_0_50 + fixed_point_t::_0_10, 0.01, 100) \
	FIXED(THROTTLE_GAIN, throttle_gain, fixed_point_t::_0_10 / 2, 0, 1) \
	FIXED(THROTTLE_LOSS, throttle_loss, fixed_point_t::_0_10, 0, 1) \
	FIXED(THROTTLE_MIN, throttle_min, fixed_point_t::_0_20, 0.01, 1) \
	RATE(MIN_AFFORDABLE_SHARE, min_affordable_share, Rate(1, 1000), 0, 1) \
	INT(SUPPLY_RESERVE_DAYS, supply_reserve_days, 10, 0, 3650) \
	/* The first day of the game every country gets the goods of the supply of its armies and fleets for this many days of their need in its */ \
	/* national stockpile (the market of the start has few of them: without it the armies had no supply for about three months) */ \
	INT(START_SUPPLY_STOCKPILE_DAYS, start_supply_stockpile_days, 90, 0, 3650) \
	/* Supplies of the army and the navy: every day a unit uses a day of the share (given per month, spread over SUPPLY_DAYS_PER_MONTH days) of */ \
	/* what its construction cost (its supply_cost goods; a good that is not in the construction cost, such as ammunition, takes the amount of */ \
	/* small_arms; a regiment uses every good of its construction cost too). The goods that are used up (the food and the drink: canned_food, */ \
	/* wine, liquor...; fuel, coal) take the consumable share, the equipment (arms, ammunition, artillery, clothes, wool, convoys) the durable one. */ \
	/* An infantry regiment (10 rifles, 10 cans to build): 0.1 rifles and 10 cans a month (1 and 100 percent: 0.033 and 3.33 percent a day) */ \
	INT(SUPPLY_DAYS_PER_MONTH, supply_days_per_month, 30, 1, 365) \
	/* The canned food the national stockpile does not have is replaced by grain, this many grain for a can (the grain feeds half as well; */ \
	/* the government buys the grain for it), 0: no replacement */ \
	FIXED(CANNED_FOOD_GRAIN_SUBSTITUTE, canned_food_grain_substitute, fixed_point_t(2), 0, 100) \
	/* The regiment types of Vic2 eat grain and fish instead of their canned food: this share of it is fish (8 grain and 2 fish for 10 cans) */ \
	RATE(ARMY_RATION_FISH_SHARE, army_ration_fish_share, Rate(20, 100), 0, 1) \
	RATE(ARMY_SUPPLY_DURABLE_SHARE, army_supply_durable_share, Rate(1, 100), 0, 10) \
	RATE(ARMY_SUPPLY_CONSUMABLE_SHARE, army_supply_consumable_share, Rate(100, 100), 0, 10) \
	RATE(NAVY_SUPPLY_DURABLE_SHARE, navy_supply_durable_share, Rate(1, 100), 0, 10) \
	RATE(NAVY_SUPPLY_CONSUMABLE_SHARE, navy_supply_consumable_share, Rate(10, 100), 0, 10) \
	/* The coal and the fuel of ships (their share replaces the consumable one): a fleet that stands at sea, one that moves, one in a port */ \
	RATE(NAVY_FUEL_SHARE_AT_SEA, navy_fuel_share_at_sea, Rate(1, 100), 0, 10) \
	RATE(NAVY_FUEL_SHARE_MOVING, navy_fuel_share_moving, Rate(10, 100), 0, 10) \
	RATE(NAVY_FUEL_SHARE_IN_PORT, navy_fuel_share_in_port, Rate(0, 100), 0, 10) \
	/* A group that moves uses this many times more of the goods (not of the coal and the fuel of ships); on the land of an enemy (a country */ \
	/* at war with the owner) every good of a group uses this many times more (both together are multiplied) */ \
	FIXED(SUPPLY_MOVING_MULTIPLIER, supply_moving_multiplier, fixed_point_t::_1 + fixed_point_t::_0_50, 0, 100) \
	FIXED(SUPPLY_ENEMY_LAND_MULTIPLIER, supply_enemy_land_multiplier, fixed_point_t::_2, 0, 100) \
	/* The supply level of the navy (1 = fully equipped) goes to the level of the budget slider times the share of the needs the stockpile */ \
	/* could give, the supply level of an army to what the supplies of its regiments let (when their reserve is used up); it changes by this */ \
	/* share a day. The attack, defence and gun power are multiplied by it */ \
	RATE(SUPPLY_LEVEL_CHANGE_PER_DAY, supply_level_change_per_day, Rate(2, 100), 0, 1) \
	INT(CONSTRUCTION_RESERVE_DAYS, construction_reserve_days, 5, 0, 3650) \
	INT(POP_START_SAVINGS_DAYS, pop_start_savings_days, 7, 0, 3650) \
	/* Investment fund: the construction slider of the budget sets aside this share of the daily income at 100 percent */ \
	RATE(INVESTMENT_BUDGET_SHARE, investment_budget_share, Rate(50, 100), 0, 1) \
	/* How often (in days) the profit of the possible enterprises and railroads is reviewed, and how many projects may start per review */ \
	INT(INVESTMENT_PERIOD_DAYS, investment_period_days, 7, 1, 365) \
	INT(INVESTMENT_PROJECTS_PER_REVIEW, investment_projects_per_review, 2, 1, 100) \
	/* A project is approved when the profit of this many days is at least INVESTMENT_MIN_RETURN of its cost */ \
	INT(INVESTMENT_PROFIT_DAYS, investment_profit_days, 182, 1, 3650) \
	RATE(INVESTMENT_MIN_RETURN, investment_min_return, Rate(10, 100), 0, 10) \
	/* The share of the jobs of a new level of a factory that the unemployed of the state must be able to fill */ \
	RATE(INVESTMENT_MIN_STAFFING, investment_min_staffing, Rate(25, 100), 0, 1) \
	/* The profit of a level of railroad: this share of the daily revenue of the resource gathering of the province */ \
	RATE(INVESTMENT_RAILROAD_BONUS, investment_railroad_bonus, Rate(20, 100), 0, 10) \
	/* Chances (weights) of the pop types to be given the money of the fund and to build the enterprise */ \
	/* The deficit (first priority of the planner): a good the country needs, makes less than this share of what it needs, and the market */ \
	/* cannot supply (the demand of the market is more than its supply by this margin) */ \
	RATE(INVESTMENT_DEFICIT_COVERAGE, investment_deficit_coverage, Rate(50, 100), 0, 1) \
	RATE(INVESTMENT_DEFICIT_MARGIN, investment_deficit_margin, Rate(10, 100), 0, 10) \
	/* The AI sets its allocation to the fund once per review so that the budget does not go into a loss: it only invests this share */ \
	/* of the average daily surplus (average of this many days) and only while the treasury holds this many days of its spending (0: no reserve) */ \
	INT(INVESTMENT_AI_AVERAGE_DAYS, investment_ai_average_days, 30, 1, 3650) \
	INT(INVESTMENT_AI_RESERVE_DAYS, investment_ai_reserve_days, 0, 0, 3650) \
	RATE(INVESTMENT_AI_SURPLUS_SHARE, investment_ai_surplus_share, Rate(75, 100), 0, 1) \
	/* The AI stops setting money aside while its fund holds the income of this many days (money that nobody uses is not saved) */ \
	INT(INVESTMENT_AI_MAX_FUND_DAYS, investment_ai_max_fund_days, 14, 1, 36500) \
	INT(INVESTMENT_WEIGHT_CAPITALISTS, investment_weight_capitalists, 50, 0, 10000) \
	INT(INVESTMENT_WEIGHT_ARISTOCRATS, investment_weight_aristocrats, 25, 0, 10000) \
	INT(INVESTMENT_WEIGHT_ARTISANS, investment_weight_artisans, 5, 0, 10000) \
	INT(INVESTMENT_WEIGHT_CLERGYMEN, investment_weight_clergymen, 5, 0, 10000) \
	INT(INVESTMENT_WEIGHT_CLERKS, investment_weight_clerks, 5, 0, 10000) \
	INT(INVESTMENT_WEIGHT_OFFICERS, investment_weight_officers, 5, 0, 10000) \
	INT(INVESTMENT_WEIGHT_BUREAUCRATS, investment_weight_bureaucrats, 5, 0, 10000) \
	RATE(ARTISAN_AVAILABILITY_WEIGHT, artisan_availability_weight, Rate(1, 1), 0, 1) \
	/* The money of the investment fund that nobody uses: when the fund holds more than this many days of the income of the country, */ \
	/* this share of the excess goes back to the treasury every day (and from there to the budget, the pops get it as salaries) */ \
	INT(INVESTMENT_FUND_MAX_DAYS, investment_fund_max_days, 14, 1, 36500) \
	RATE(INVESTMENT_FUND_RELEASE_SHARE, investment_fund_release_share, Rate(10, 100), 0, 1) \
	/* Wood: what a pop of 200000 needs of timber and lumber per day as an everyday need (the original game has none; food is about 100 times more) */ \
	FIXED(POP_TIMBER_NEED, pop_timber_need, fixed_point_t(25) / 1000, 0, 100) \
	FIXED(POP_LUMBER_NEED, pop_lumber_need, fixed_point_t(25) / 1000, 0, 100) \
	/* Every building that needs cement needs this much lumber for each unit of cement, and this much timber on top (the construction goods) */ \
	FIXED(LUMBER_PER_CEMENT, lumber_per_cement, fixed_point_t::_0_50, 0, 100) \
	INT(TIMBER_PER_CEMENT_BUILDING, timber_per_cement_building, 100, 0, 100000) \
	/* A factory is checked for expansion every this many months from its completion: it is expanded when its workplaces were filled */ \
	/* (EXPANSION_MIN_EMPLOYMENT) in every one of the months since the last check */ \
	INT(EXPANSION_CHECK_MONTHS, expansion_check_months, 3, 1, 120) \
	/* The output of the fishing grounds (the fish RGO) of the whole world is multiplied by this once after the loading: fish was always cheap, 1 = the game as it is */ \
	/* (2.2: twice, and 10 % more for the rations of the armies); the same for the grain, the wool and the cattle (10 % more, decision of the owner 2026-11-07) */ \
	FIXED(FISH_OUTPUT_MULTIPLIER, fish_output_multiplier, fixed_point_t(22) / 10, 0.1, 20) \
	FIXED(GRAIN_OUTPUT_MULTIPLIER, grain_output_multiplier, fixed_point_t(11) / 10, 0.1, 20) \
	FIXED(WOOL_OUTPUT_MULTIPLIER, wool_output_multiplier, fixed_point_t(11) / 10, 0.1, 20) \
	FIXED(CATTLE_OUTPUT_MULTIPLIER, cattle_output_multiplier, fixed_point_t(11) / 10, 0.1, 20) \
	/* Every ship has this much lumber in its build cost for each piece of the rest of the cost (clipper_convoy, artillery...): man-of-war 25 pieces x 4 = 100 lumber, */ \
	/* and it needs 1 % of it every month (NAVY_SUPPLY_DURABLE_SHARE). 0 = the ships need no lumber */ \
	FIXED(SHIP_LUMBER_PER_PIECE, ship_lumber_per_piece, fixed_point_t(4), 0, 100) \
	/* The buildings of the RGOs (docs/RGO_BUILDINGS.md): a level gives the staff and the output of the RGO this much more; max level, days and lumber are the */ \
	/* defaults of a building (its file may name its own); STOCK_STARTS: the lumber of this many starts the government keeps in the stockpile; STARTS_PER_MONTH */ \
	/* is the most a country starts in a month. RGO_BUILDING_ENABLED 0 turns the buildings off */ \
	INT(RGO_BUILDING_ENABLED, rgo_building_enabled, 1, 0, 1) \
	RATE(RGO_BUILDING_STAFF_PER_LEVEL, rgo_building_staff_per_level, Rate(5, 100), 0, 10) \
	RATE(RGO_BUILDING_OUTPUT_PER_LEVEL, rgo_building_output_per_level, Rate(10, 100), 0, 10) \
	/* The pops build a level when the unemployed of the job pop types of the RGO are at least this share of its workers */ \
	RATE(RGO_BUILDING_MIN_UNEMPLOYMENT, rgo_building_min_unemployment, Rate(1, 100), 0, 1) \
	/* The land units use this share of the wool, clothes, luxury clothes and cattle of their cost and supply of the files (the troops took too much) */ \
	RATE(ARMY_CLOTH_CATTLE_SHARE, army_cloth_cattle_share, Rate(50, 100), 0, 1) \
	INT(RGO_BUILDING_MAX_LEVEL, rgo_building_max_level, 2, 0, 100) \
	INT(RGO_BUILDING_BUILD_DAYS, rgo_building_build_days, 90, 1, 3650) \
	FIXED(RGO_BUILDING_LUMBER, rgo_building_lumber, fixed_point_t(10), 0, 100000) \
	INT(RGO_BUILDING_STOCK_STARTS, rgo_building_stock_starts, 4, 0, 1000) \
	INT(RGO_BUILDING_STARTS_PER_MONTH, rgo_building_starts_per_month, 30, 0, 10000)
	// Credits (docs/CREDITS.md). Lengths and delays are in days; the delay is the time before the first payment.
#define OV_CREDIT_DEFINES(INT, FIXED, RATE, DOUBLE) \
	INT(TERM_SHORT_DAYS, term_short_days, 1825, 30, 36500) \
	INT(DELAY_SHORT_DAYS, delay_short_days, 913, 0, 36500) \
	INT(TERM_MEDIUM_DAYS, term_medium_days, 3650, 30, 36500) \
	INT(DELAY_MEDIUM_DAYS, delay_medium_days, 1825, 0, 36500) \
	INT(TERM_LONG_DAYS, term_long_days, 7300, 30, 36500) \
	INT(DELAY_LONG_DAYS, delay_long_days, 3650, 0, 36500) \
	INT(SHORT_MAX_CONSTRUCTION_DAYS, short_max_construction_days, 365, 1, 36500) \
	RATE(INTEREST_MIN, interest_min, Rate(1, 100), 0, 100) \
	RATE(INTEREST_MAX, interest_max, Rate(25, 100), 0, 100) \
	RATE(INTEREST_LIMIT, interest_limit, Rate(1, 1), 0, 100) \
	INT(MIN_LOAN, min_loan, 100, 1, 100000000) \
	INT(DEBT_LIMIT_BASE, debt_limit_base, 5000, 0, 100000000) \
	INT(DEBT_LIMIT_POP_DIVISOR, debt_limit_pop_divisor, 100, 1, 100000000) \
	INT(AI_OFFER_EXPIRY_DAYS, ai_offer_expiry_days, 90, 1, 36500) \
	INT(AI_LEND_RESERVE, ai_lend_reserve, 1000, 0, 100000000) \
	RATE(AI_LEND_SHARE, ai_lend_share, Rate(3, 10), 0, 1) \
	INT(AI_MAX_LOAN, ai_max_loan, 25000, 0, 100000000) \
	INT(AI_NEEDS_MONEY_BELOW, ai_needs_money_below, 3000, 0, 100000000) \
	INT(AI_INTERNAL_LEND_CASH, ai_internal_lend_cash, 4000, 0, 100000000) \
	RATE(AI_INTERNAL_LEND_SHARE, ai_internal_lend_share, Rate(1, 10), 0, 1) \
	INT(AI_INTERNAL_MAX_LOAN, ai_internal_max_loan, 5000, 0, 100000000) \
	INT(AI_MIN_RELATIONS, ai_min_relations, 0, -200, 200) \
	INT(AI_MAX_OPEN_OFFERS, ai_max_open_offers, 1, 0, 100) \
	RATE(AI_LENDER_MAX_DEBT_RATIO, ai_lender_max_debt_ratio, Rate(8, 10), 0, 10) \
	RATE(AI_RELATIONS_RATE_PER_POINT, ai_relations_rate_per_point, Rate(1, 10000), 0, 1) \
	INT(AI_EARLY_REPAY_MARGIN, ai_early_repay_margin, 3000, 0, 100000000) \
	INT(INVESTMENT_MAX_PAYBACK_YEARS, investment_max_payback_years, 12, 1, 1000) \
	INT(INVESTMENT_MIN_MONEY, investment_min_money, 500, 0, 100000000)

	// Trade (docs/TRADE.md). Freight is the share of the price that the seller loses on the way to the buyer; the money goes to the
	// treasury of the seller's country (the state keeps the railroads and the ports). Capacities are in pounds of cargo value per day,
	// for every good separately (the quantity is the value divided by the price of the good).
#define OV_TRADE_DEFINES(INT, FIXED, RATE, DOUBLE) \
	/* 0: the old market (everything is sold at one price, exports are shared evenly, nothing is lost on the way) */ \
	INT(ENABLED, enabled, 1, 0, 1) \
	/* Inside a country: the freight of a seller without any railroad (maximum) and of one in a province with the best railroad (minimum) */ \
	RATE(INTERNAL_FREIGHT_MAX, internal_freight_max, Rate(15, 1000), 0, 1) \
	RATE(INTERNAL_FREIGHT_MIN, internal_freight_min, Rate(3, 1000), 0, 1) \
	/* Land borders: the freight of one crossing, the part of it that the best railroads on both sides of the border remove, */ \
	/* and the capacity of a crossing (a pair of neighbouring provinces of two countries) without and with railroads */ \
	RATE(LAND_COST_BASE, land_cost_base, Rate(1, 100), 0, 1) \
	RATE(LAND_RAIL_COST_RELIEF, land_rail_cost_relief, Rate(60, 100), 0, 1) \
	INT(LAND_CAPACITY_BASE, land_capacity_base, 400, 0, 100000000) \
	INT(LAND_CAPACITY_PER_RAIL_LEVEL, land_capacity_per_rail_level, 200, 0, 100000000) \
	/* Sea: every country with a port can trade by sea with every other one. The freight is the base plus this share for a distance */ \
	/* of the width of the map, naval bases remove up to SEA_NAVAL_BASE_COST_RELIEF of it (full at the average level of */ \
	/* SEA_NAVAL_BASE_RELIEF_LEVELS). The capacity of a country is the sum over its ports; a pair of countries uses SEA_PAIR_SHARE of the smaller one */ \
	RATE(SEA_COST_BASE, sea_cost_base, Rate(1, 100), 0, 1) \
	RATE(SEA_COST_PER_MAP_WIDTH, sea_cost_per_map_width, Rate(6, 100), 0, 10) \
	RATE(SEA_NAVAL_BASE_COST_RELIEF, sea_naval_base_cost_relief, Rate(40, 100), 0, 1) \
	INT(SEA_NAVAL_BASE_RELIEF_LEVELS, sea_naval_base_relief_levels, 4, 1, 100) \
	INT(SEA_PORT_BASE_CAPACITY, sea_port_base_capacity, 400, 0, 100000000) \
	INT(SEA_CAPACITY_PER_NAVAL_BASE_LEVEL, sea_capacity_per_naval_base_level, 400, 0, 100000000) \
	RATE(SEA_PAIR_SHARE, sea_pair_share, Rate(50, 100), 0, 1) \
	/* Routes through other countries: at most this many borders/seas, with this extra freight for every country passed */ \
	INT(MAX_ROUTE_HOPS, max_route_hops, 3, 1, 10) \
	RATE(TRANSIT_COST_PER_HOP, transit_cost_per_hop, Rate(5, 1000), 0, 1) \
	/* A route that costs more than this share of the price is not used */ \
	RATE(MAX_ROUTE_COST, max_route_cost, Rate(25, 100), 0, 1) \
	/* Trade with and from provinces nobody owns (not a country) and trade that the plan of the day did not foresee: freight */ \
	RATE(OUTSIDE_COST, outside_cost, Rate(4, 100), 0, 1) \
	RATE(SPOT_COST, spot_cost, Rate(5, 100), 0, 1) \
	/* The share of the freight that goes to the investment fund of the country (the rest goes to the treasury) */ \
	RATE(FREIGHT_FUND_SHARE, freight_fund_share, Rate(0, 100), 0, 1) \
	/* Subsistence: the workers of a resource gathering operation keep this share of its output for themselves (only goods that they */ \
	/* need), without a market, at no freight; railroads in the province turn the farms to the market: at the best railroad the share */ \
	/* is reduced by SUBSISTENCE_RAIL_RELIEF */ \
	RATE(SUBSISTENCE_SHARE, subsistence_share, Rate(35, 100), 0, 1) \
	RATE(SUBSISTENCE_RAIL_RELIEF, subsistence_rail_relief, Rate(70, 100), 0, 1) \
	/* The workers eat the goods they keep from the quantity that they need at most: the base need of the goods they eat times this */ \
	INT(SUBSISTENCE_NEED_MULTIPLIER, subsistence_need_multiplier, 2, 1, 100)

	// Supply limit of the provinces (docs/PLAN_ARMY_SUPPLY_FRONTS.md): how many regiments a province can feed. The limit of a province is
	// (what the files give it: terrain, land_province, climate, buildings + the global supply_limit of the country of the army) times the
	// railroad times the technologies times the bonus of the own land. The map mode "supply" colours the provinces by limit / load
#define OV_SUPPLY_DEFINES(INT, FIXED, RATE, DOUBLE) \
	/* The limit that the files give, times this (1 = as in the files) */ \
	FIXED(SUPPLY_LIMIT_BASE, supply_limit_base, fixed_point_t::_1, 0, 100) \
	/* Every level of the railroad of the province adds this share to the limit (the levels are added: 3 levels = +30 percent) */ \
	RATE(SUPPLY_RAIL_BONUS_PER_LEVEL, supply_rail_bonus_per_level, Rate(10, 100), 0, 10) \
	/* On the own land of the army (the province is owned by the country of the army: not allied, not captured) the limit is higher by this */ \
	/* share; the second share is what the technology that is not in the game yet will give */ \
	RATE(SUPPLY_HOME_BONUS, supply_home_bonus, Rate(5, 100), 0, 10) \
	RATE(SUPPLY_HOME_BONUS_TECH, supply_home_bonus_tech, Rate(5, 100), 0, 10) \
	/* The reserve of every regiment, in days of its supply, comes from the technologies (without them it is 0): the organisation of the army */ \
	/* (the civilised countries have it from the start) and the deep strikes and assaults give this many days each, a late technology one more */ \
	/* day: a week in all. The days */ \
	/* are counted for the need on the land of an enemy: a regiment without any supply stands that many days, then its supply level falls. */ \
	/* Without a delivery (shortfall) the reserve falls by the days of the shortfall; with a full delivery it rises by the refill (days a day) */ \
	INT(SUPPLY_UNIT_RESERVE_DAYS_ORGANISATION, supply_unit_reserve_days_organisation, 3, 0, 365) \
	INT(SUPPLY_UNIT_RESERVE_DAYS_DEEP_ASSAULT, supply_unit_reserve_days_deep_assault, 3, 0, 365) \
	INT(SUPPLY_UNIT_RESERVE_DAYS_LATE, supply_unit_reserve_days_late, 1, 0, 365) \
	FIXED(SUPPLY_UNIT_RESERVE_REFILL_PER_DAY, supply_unit_reserve_refill_per_day, fixed_point_t::_1, 0, 100) \
	/* The priority of the supply of an army: the increased priority sends this many times more than the army needs (but not more than */ \
	/* what covers the losses), the maximum priority this many times more */ \
	FIXED(SUPPLY_PRIORITY_INCREASED_MAX, supply_priority_increased_max, fixed_point_t::_2 + fixed_point_t::_0_50, 1, 100) \
	FIXED(SUPPLY_PRIORITY_MAX_MAX, supply_priority_max_max, fixed_point_t(5), 1, 100) \
	/* A province that cannot feed the regiments in it takes the free supply limit of its neighbours (that are held by the country or its */ \
	/* friends) with this efficiency */ \
	RATE(SUPPLY_NEIGHBOUR_EFFICIENCY, supply_neighbour_efficiency, Rate(75, 100), 0, 1) \
	/* The bonus of a general to the supply of his army (the third factor of the supply limit): the traits that reduce the attrition give the */ \
	/* attrition times this, every point of prestige gives this share. A field marshal gives his bonus times SUPPLY_MARSHAL_SHARE to all the */ \
	/* armies of his group */ \
	FIXED(SUPPLY_LEADER_ATTRITION_FACTOR, supply_leader_attrition_factor, fixed_point_t::_1, 0, 100) \
	RATE(SUPPLY_LEADER_PRESTIGE_BONUS, supply_leader_prestige_bonus, Rate(0, 100), 0, 1) \
	RATE(SUPPLY_MARSHAL_SHARE, supply_marshal_share, Rate(50, 100), 0, 1) \
	/* An army that gets at least this share of its need is fully supplied (its reserve neither falls nor needs more than it gets) */ \
	RATE(SUPPLY_FULL_THRESHOLD, supply_full_threshold, Rate(99, 100), 0, 1) \
	/* The supply level of an army below this share gives the message "the army is without supplies" */ \
	RATE(SUPPLY_ALERT_LEVEL, supply_alert_level, Rate(50, 100), 0, 1) \
	/* The supply coefficient of the armies on the land that is not held by the country or a friend (an ally, a country on the same side of a */ \
	/* war, a country that gave the passage). One neighbour province is held: the first number (the technology adds the second); two or */ \
	/* more neighbours are held: 100 percent. No neighbour is held, but one province lies between: the base for the first held province */ \
	/* that touches the provinces between and the step for every next one (at most the count of them is counted). The same for two */ \
	/* provinces between, which the technology "modern armies" opens (SUPPLY_GAP2_ENABLED = 1 opens it without the technology). Further */ \
	/* away: SUPPLY_FAR (the technology adds). The generals do not change these numbers, only the technologies do */ \
	RATE(SUPPLY_NEIGHBOUR_ONE, supply_neighbour_one, Rate(90, 100), 0, 1) \
	RATE(SUPPLY_NEIGHBOUR_ONE_TECH, supply_neighbour_one_tech, Rate(10, 100), 0, 1) \
	RATE(SUPPLY_GAP1_BASE, supply_gap1_base, Rate(50, 100), 0, 1) \
	RATE(SUPPLY_GAP1_STEP, supply_gap1_step, Rate(10, 100), 0, 1) \
	INT(SUPPLY_GAP1_MAX_COUNT, supply_gap1_max_count, 4, 1, 100) \
	INT(SUPPLY_GAP2_ENABLED, supply_gap2_enabled, 0, 0, 1) \
	RATE(SUPPLY_GAP2_BASE, supply_gap2_base, Rate(25, 100), 0, 1) \
	RATE(SUPPLY_GAP2_STEP, supply_gap2_step, Rate(5, 100), 0, 1) \
	INT(SUPPLY_GAP2_MAX_COUNT, supply_gap2_max_count, 5, 1, 100) \
	RATE(SUPPLY_FAR, supply_far, Rate(10, 100), 0, 1) \
	RATE(SUPPLY_FAR_TECH, supply_far_tech, Rate(10, 100), 0, 1) \
	/* The map mode "supply" for a province without an army: the load is this many regiments (a standard division) */ \
	FIXED(SUPPLY_MAP_REFERENCE_LOAD, supply_map_reference_load, fixed_point_t(4), 0, 1000) \
	/* Colours of the map mode "supply" by limit / load. At least the first: dark green; above the second: green; above the third: light */ \
	/* blue; at least the fourth: blue; below 1 (a shortage) at least the fifth: yellow, the sixth: orange, the seventh: red; less: dark red */ \
	RATE(SUPPLY_MAP_DARK_GREEN_FROM, supply_map_dark_green_from, Rate(150, 100), 0, 100) \
	RATE(SUPPLY_MAP_GREEN_ABOVE, supply_map_green_above, Rate(125, 100), 0, 100) \
	RATE(SUPPLY_MAP_LIGHT_BLUE_ABOVE, supply_map_light_blue_above, Rate(110, 100), 0, 100) \
	RATE(SUPPLY_MAP_BLUE_FROM, supply_map_blue_from, Rate(100, 100), 0, 100) \
	RATE(SUPPLY_MAP_YELLOW_FROM, supply_map_yellow_from, Rate(90, 100), 0, 100) \
	RATE(SUPPLY_MAP_ORANGE_FROM, supply_map_orange_from, Rate(75, 100), 0, 100) \
	RATE(SUPPLY_MAP_RED_FROM, supply_map_red_from, Rate(50, 100), 0, 100)

	OV_DEFINE_GROUP(OpenVicAIDefines, "ov_ai", OV_AI_DEFINES)
	OV_DEFINE_GROUP(OpenVicPopulationDefines, "ov_population", OV_POPULATION_DEFINES)
	OV_DEFINE_GROUP(OpenVicPoliticsDefines, "ov_politics", OV_POLITICS_DEFINES)
	OV_DEFINE_GROUP(OpenVicColonisationDefines, "ov_colonisation", OV_COLONISATION_DEFINES)
	OV_DEFINE_GROUP(OpenVicWarDefines, "ov_war", OV_WAR_DEFINES)
	OV_DEFINE_GROUP(OpenVicPeaceDefines, "ov_peace", OV_PEACE_DEFINES)
	OV_DEFINE_GROUP(OpenVicResearchDefines, "ov_research", OV_RESEARCH_DEFINES)
	OV_DEFINE_GROUP(OpenVicEconomyDefines, "ov_economy", OV_ECONOMY_DEFINES)
	OV_DEFINE_GROUP(OpenVicCreditDefines, "ov_credit", OV_CREDIT_DEFINES)
	OV_DEFINE_GROUP(OpenVicTradeDefines, "ov_trade", OV_TRADE_DEFINES)
	OV_DEFINE_GROUP(OpenVicSupplyDefines, "ov_supply", OV_SUPPLY_DEFINES)

	struct OpenVicDefines {
	private:
		std::uint64_t hash = 0;
		bool file_loaded = false;

	public:
		// Computes the hash again after values were changed (load_file does it itself)
		void update_hash();

		OpenVicAIDefines ai;
		OpenVicPopulationDefines population;
		OpenVicPoliticsDefines politics;
		OpenVicColonisationDefines colonisation;
		OpenVicWarDefines war;
		OpenVicPeaceDefines peace;
		OpenVicResearchDefines research;
		OpenVicEconomyDefines economy;
		OpenVicCreditDefines credit;
		OpenVicTradeDefines trade;
		OpenVicSupplyDefines supply;

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
		// The text of an example common/openvic_defines.lua with every key, its default (the current value) and its range
		std::string to_lua_text() const;
		// The groups in a fixed order
		std::vector<OpenVicDefineGroup*> get_groups();
		std::vector<OpenVicDefineGroup const*> get_groups() const;
	};
}

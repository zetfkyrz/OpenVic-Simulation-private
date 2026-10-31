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
	RATE(SCORE_LIFE_NEEDS_WEIGHT, score_life_needs_weight, Rate(1, 2), 0, 1) \
	/* Surplus people: the unemployed of a pop type that can be unemployed. A pop is hungry when it has less than this share of its life */ \
	/* needs; the surplus of a pop that was hungry for HUNGER_DAYS_BEFORE_DEATH days of the month emigrates (EMIGRATION_SHARE of it, */ \
	/* the technologies raise it later) and the rest dies (SURPLUS_DEATH_SHARE of the rest, per month) */ \
	RATE(HUNGER_LIFE_NEEDS_THRESHOLD, hunger_life_needs_threshold, Rate(50, 100), 0, 1) \
	INT(HUNGER_DAYS_BEFORE_DEATH, hunger_days_before_death, 28, 1, 31) \
	RATE(EMIGRATION_SHARE, emigration_share, Rate(1, 100), 0, 1) \
	RATE(SURPLUS_DEATH_SHARE, surplus_death_share, Rate(99, 100), 0, 1) \
	/* The surplus moves inside the country to provinces with free jobs of the same pop type, and abroad only when the whole country */ \
	/* has no free jobs; at most this share of the free jobs of a destination is filled per month */ \
	RATE(IMMIGRATION_FILL_SHARE, immigration_fill_share, Rate(50, 100), 0, 1) \
	INT(STARVATION_MIN_SURPLUS, starvation_min_surplus, 1, 0, 1000000)
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
	INT(DIG_IN_DIVISOR, dig_in_divisor, 10, 1, 1000) \
	INT(LEADER_DEATH_ODDS, leader_death_odds, 30, 1, 1000000) \
	RATE(LEADER_VICTORY_PRESTIGE, leader_victory_prestige, Rate(1, 100), 0, 100) \
	RATE(LEADER_DEFEAT_PRESTIGE, leader_defeat_prestige, Rate(1, 100), 0, 100) \
	RATE(MIN_ORGANISATION_FACTOR, min_organisation_factor, Rate(3, 10), 0, 1) \
	RATE(DEFENCE_FACTOR_MIN, defence_factor_min, Rate(1, 2), 0.01, 100) \
	INT(DEFENCE_VALUE_DIVISOR, defence_value_divisor, 2, 1, 1000) \
	RATE(ORGANISATION_DAMAGE, organisation_damage, Rate(5, 100), 0, 100) \
	RATE(STRENGTH_DAMAGE, strength_damage, Rate(1, 100), 0, 100) \
	INT(ROUT_STRENGTH_MULTIPLIER, rout_strength_multiplier, 3, 1, 1000) \
	RATE(ROUT_THRESHOLD, rout_threshold, Rate(1, 10), 0, 1) \
	RATE(SIEGE_SPEED_PER_REGIMENT, siege_speed_per_regiment, Rate(1, 8), 0, 100) \
	INT(SIEGE_SKILL_DIVISOR, siege_skill_divisor, 10, 1, 1000) \
	INT(SIEGE_SPEED_MAX, siege_speed_max, 4, 1, 1000) \
	RATE(ORGANISATION_RECOVERY, organisation_recovery, Rate(2, 100), 0, 1) \
	RATE(STRENGTH_RECOVERY, strength_recovery, Rate(5, 1000), 0, 1) \
	RATE(RECOVERY_MIN_SUPPLY, recovery_min_supply, Rate(1, 4), 0, 1) \
	RATE(STRENGTH_RECOVERY_MIN_SUPPLY, strength_recovery_min_supply, Rate(1, 2), 0, 1) \
	RATE(NAVAL_STRENGTH_DAMAGE, naval_strength_damage, Rate(2, 100), 0, 100) \
	RATE(NAVAL_ORGANISATION_DAMAGE, naval_organisation_damage, Rate(8, 100), 0, 100) \
	INT(NAVAL_EVASION_DIVISOR, naval_evasion_divisor, 10, 1, 1000) \
	INT(NAVAL_MAX_DAMAGE_RATIO, naval_max_damage_ratio, 6, 1, 1000) \
	INT(FRONT_SPREAD_CHUNK_SIZE, front_spread_chunk_size, 4, 1, 40) \
	INT(FRONT_SPREAD_MAX_SPLITS_PER_DAY, front_spread_max_splits_per_day, 4, 0, 100) \
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
	/* templates by the year of the game; 0 (the default) when the technologies of the army give them (TEMPORARY, docs/PENDING_PLANS.md) */ \
	INT(PROVISIONAL_UNLOCKS_ENABLED, provisional_unlocks_enabled, 0, 0, 1) \
	/* What a template is worth (military/DivisionStats.cpp): the men of a strength point of a regiment, the kit and the men weigh this many */ \
	/* kilograms per strength point, the occupation speed 1 is the base (technologies add to it, from this percent it is instant) */ \
	INT(TEMPLATE_MEN_PER_STRENGTH, template_men_per_strength, 1000, 1, 100000) \
	INT(TEMPLATE_KIT_WEIGHT_PER_STRENGTH, template_kit_weight_per_strength, 100000, 0, 10000000) \
	/* The occupation of a province (military/BattleFormulas.cpp): the power of a division is the sum of the occupation weights of its regiments over */ \
	/* OCCUPATION_REFERENCE_REGIMENTS (6 = 3 ranks of 2); the days are the base days over (power x (1 + the sum of occupation_speed_pct) x the product of */ \
	/* occupation_speed_mult); OCCUPATION_BASE_DAYS is the base of a plain province in the dump of the templates (the siege has its own days) */ \
	INT(OCCUPATION_REFERENCE_REGIMENTS, occupation_reference_regiments, 6, 1, 1000) \
	INT(OCCUPATION_BASE_DAYS, occupation_base_days, 10, 1, 3650) \
	/* The armies (their generals) that can be attached to the field marshal of a group; the technologies will raise it */ \
	INT(MARSHAL_MAX_ATTACHED, marshal_max_attached, 8, 1, 64) \
	/* 1: the transports carry any number of regiments (for now); 0: REGIMENTS_PER_TRANSPORT of every transport ship */ \
	INT(TRANSPORT_UNLIMITED, transport_unlimited, 1, 0, 1) \
	/* The battle of divisions (military/BattleFormulas.cpp, replaceable): durability points lost in a day per point of attack, the defence that */ \
	/* halves the damage, the damage added by the density of the formation per point of density */ \
	/* The weapons of the regiments (military/RegimentEquipment in CountryInstance.cpp): how many regiments a day get a weapon of a better */ \
	/* generation out of the national stockpile (the government buys the goods for them), and the share of the attack, the defence and the */ \
	/* breakthrough a unit loses for every generation below the lowest weapon its type may have, when the country has none of them */ \
	INT(EQUIPMENT_UPGRADES_PER_DAY, equipment_upgrades_per_day, 1, 0, 1000) \
	RATE(EQUIPMENT_BELOW_MIN_PENALTY, equipment_below_min_penalty, Rate(25, 100), 0, 1) \
	/* A level of the fortification adds this share to the defence */ \
	RATE(FORTIFICATION_PER_LEVEL, fortification_per_level, Rate(5, 100), 0, 10) \
	FIXED(BATTLE_DAMAGE_PER_ATTACK, battle_damage_per_attack, fixed_point_t::_0_10, 0, 1000) \
	FIXED(BATTLE_DEFENCE_REFERENCE, battle_defence_reference, fixed_point_t(20), 1, 100000) \
	/* The density of a division (military/BattleFormulas.cpp): density = DENSITY_DEPTH_FACTOR x depth / width (a width twice the depth is 1.0); x = |1 - density|; */ \
	/* the penalty is DENSITY_LINEAR x x + DENSITY_QUADRATIC x x^2 (the rule of the owner is x^2 + x). Below 1 the defence and the breakthrough fall by the penalty; */ \
	/* above 1 the casualties and the breakthrough grow by it and the attack by DENSITY_ATTACK_SHARE of it */ \
	FIXED(DENSITY_DEPTH_FACTOR, density_depth_factor, fixed_point_t(2), 0.01, 100) \
	FIXED(DENSITY_LINEAR, density_linear, fixed_point_t::_1, 0, 10) \
	FIXED(DENSITY_QUADRATIC, density_quadratic, fixed_point_t::_1, 0, 10) \
	RATE(DENSITY_ATTACK_SHARE, density_attack_share, Rate(1, 2), 0, 1) \
	/* The breakthrough (R = the breakthrough of the attacker / the effective defence of the defender): R of BREAK_RATIO_DAY in one day, or R of */ \
	/* BREAK_RATIO_WEEK for BREAK_WEEK_DAYS days in a row, makes the defence retreat. The defence of the attacker softens the damage it takes by */ \
	/* ATTACKER_DEFENSE_FACTOR (0: not at all) */ \
	RATE(ATTACKER_DEFENSE_FACTOR, attacker_defense_factor, Rate(0, 100), 0, 1) \
	FIXED(BREAK_RATIO_DAY, break_ratio_day, fixed_point_t(3), 0, 1000) \
	FIXED(BREAK_RATIO_WEEK, break_ratio_week, fixed_point_t(2), 0, 1000) \
	INT(BREAK_WEEK_DAYS, break_week_days, 7, 1, 365) \
	RATE(ATTRITION_BASE, attrition_base, Rate(2, 1000), 0, 1) \
	RATE(ATTRITION_NO_SUPPLY, attrition_no_supply, Rate(3, 1000), 0, 1) \
	RATE(ATTRITION_FLOOR_STRENGTH, attrition_floor_strength, Rate(1, 5), 0, 1) \
	RATE(ATTRITION_FRIENDLY_MULTIPLIER, attrition_friendly_multiplier, Rate(1, 2), 0, 1) \
	INT(BLOCKADE_SUPPLY_DIVISOR, blockade_supply_divisor, 2, 1, 1000) \
	INT(MOVEMENT_SPEED_SCALE, movement_speed_scale, 3, 1, 1000) \
	INT(PORT_STEP_DISTANCE, port_step_distance, 10, 1, 10000) \
	INT(REGIMENTS_PER_TRANSPORT, regiments_per_transport, 4, 0, 1000) \
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
	/* The technologies of the army (ov_data/common/ov_technology): 1: only the civilised countries research them (the uncivilised have none) */ \
	INT(ARMY_RESEARCH_CIVILISED_ONLY, army_research_civilised_only, 1, 0, 1) \
	/* 1: the AI countries start the available technology with the lowest year_hint when their slot of a branch is free */ \
	INT(ARMY_RESEARCH_AI_ENABLED, army_research_ai_enabled, 1, 0, 1)
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
	/* Supplies of the army and the navy: every day a unit uses a day of the share (given per month, spread over SUPPLY_DAYS_PER_MONTH days) of */ \
	/* what its construction cost (its supply_cost goods; a good that is not in the construction cost, such as ammunition, takes the amount of */ \
	/* small_arms). The goods that are used up (canned_food, fuel, coal) take the consumable share, the equipment (arms, ammunition, artillery, */ \
	/* convoys) the durable one. An infantry regiment (10 rifles, 10 cans to build): 0.1 rifles and 1 can a month (1 and 10 percent) */ \
	INT(SUPPLY_DAYS_PER_MONTH, supply_days_per_month, 30, 1, 365) \
	RATE(ARMY_SUPPLY_DURABLE_SHARE, army_supply_durable_share, Rate(1, 100), 0, 10) \
	RATE(ARMY_SUPPLY_CONSUMABLE_SHARE, army_supply_consumable_share, Rate(10, 100), 0, 10) \
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
	INT(EXPANSION_CHECK_MONTHS, expansion_check_months, 3, 1, 120)
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

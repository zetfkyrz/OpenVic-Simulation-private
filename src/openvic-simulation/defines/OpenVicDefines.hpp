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
	INT(SIEGE_BASE_DAYS, siege_base_days, 3, 0, 36500) \
	INT(SIEGE_DAYS_PER_FORT_LEVEL, siege_days_per_fort_level, 5, 0, 36500) \
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
	RATE(INVENTION_LITERACY_CHANCE, invention_literacy_chance, Rate(6, 100), 0, 1)
#define OV_ECONOMY_DEFINES(INT, FIXED, RATE, DOUBLE) \
	INT(MAX_FACTORIES_PER_STATE, max_factories_per_state, 8, 1, 64) \
	INT(FACTORY_VALUE_PAYBACK_YEARS, factory_value_payback_years, 6, 1, 1000) \
	INT(FACTORY_REPLACEMENT_VALUE_DIVISOR, factory_replacement_value_divisor, 4, 1, 1000) \
	INT(DEFAULT_CONSTRUCTION_DAYS, default_construction_days, 180, 1, 36500) \
	RATE(STARTING_BUDGET_SHARE, starting_budget_share, Rate(5, 100), 0, 10) \
	RATE(CAPITALIST_SAVINGS_SHARE, capitalist_savings_share, Rate(3, 10), 0, 1) \
	INT(STOCKPILE_TARGET_DAYS, stockpile_target_days, 2, 0, 365) \
	FIXED(EXPANSION_FUND_SHARE, expansion_fund_share, fixed_point_t::_0_10 * 2, 0, 1) \
	RATE(EXPANSION_MIN_EMPLOYMENT, expansion_min_employment, Rate(80, 100), 0, 1) \
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
	INT(CONSTRUCTION_RESERVE_DAYS, construction_reserve_days, 5, 0, 3650)
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

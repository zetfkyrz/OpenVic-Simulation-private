#pragma once

#include <atomic>
#include <cstddef>
#include <functional>
#include <utility>

#include <fmt/base.h>

#include "openvic-simulation/defines/OpenVicDefines.hpp"
#include "openvic-simulation/misc/MoneyLedger.hpp"
#include "openvic-simulation/core/memory/SmartPtr.hpp"
#include "openvic-simulation/core/memory/Vector.hpp"
#include "openvic-simulation/core/object/Date.hpp"
#include "openvic-simulation/core/object/FixedPoint.hpp"
#include "openvic-simulation/core/object/FixedPoint/Atomic.hpp"
#include "openvic-simulation/core/object/Timespan.hpp"
#include "openvic-simulation/core/stl/containers/TypedSpan.hpp"
#include "openvic-simulation/core/thread/SpinMutex.hpp"
#include "openvic-simulation/diplomacy/CountryRelation.hpp"
#include "openvic-simulation/economy/BuildingLevel.hpp"
#include "openvic-simulation/economy/BuildingRestrictionCategory.hpp"
#include "openvic-simulation/military/CombatWidth.hpp"
#include "openvic-simulation/military/Division.hpp"
#include "openvic-simulation/military/SupplyPriority.hpp"
#include "openvic-simulation/military/UnitBranchedGetterMacro.hpp"
#include "openvic-simulation/modifier/ModifierSum.hpp"
#include "openvic-simulation/politics/RuleSet.hpp"
#include "openvic-simulation/population/PopsAggregate.hpp"
#include "openvic-simulation/research/TechnologyUnlockLevel.hpp"
#include "openvic-simulation/types/ClampedValue.hpp"
#include "openvic-simulation/types/FlagStrings.hpp"
#include "openvic-simulation/types/HasIndex.hpp"
#include "openvic-simulation/types/OrderedContainers.hpp"
#include "openvic-simulation/types/TypedIndices.hpp"
#include "openvic-simulation/types/UnitBranchType.hpp"
#include "openvic-simulation/types/UnitVariant.hpp"
#include "openvic-simulation/types/ValueHistory.hpp"
#include "openvic-simulation/utility/Getters.hpp"
#include "openvic-simulation/utility/reactive/DerivedState.hpp"
#include "openvic-simulation/utility/reactive/MutableState.hpp"

namespace OpenVic {
	struct BaseIssue;
	struct BuildingType;
	struct BuyResult;
	struct CountryDefinition;
	struct CountryDefines;
	struct CountryHistoryEntry;
	struct CountryInstanceManager;
	struct CountryInstanceDeps;
	struct CountryParty;
	struct CountryRelationManager;
	struct Crime;
	struct Culture;
	struct DefineManager;
	struct DiplomacyDefines;
	struct EconomyDefines;
	struct GameRulesManager;
	struct GoodDefinition;
	struct GoodInstance;
	struct GoodInstanceManager;
	struct GovernmentType;
	struct Ideology;
	struct Invention;
	struct LeaderInstance;
	struct MapInstance;
	struct MarketInstance;
	struct MilitaryDefines;
	struct ModifierEffectCache;
	struct NationalValue;
	struct Pop;
	struct PopType;
	struct PopsDefines;
	struct ProductionType;
	struct ProvinceInstance;
	struct Reform;
	struct ReformGroup;
	struct Religion;
	struct SellResult;
	struct SharedCountryValues;
	struct SharedPopTypeValues;
	struct State;
	struct StaticModifierCache;
	struct Strata;
	struct Technology;
	struct TechnologySchool;
	struct UnitInstanceGroup;
	struct UnitTypeManager;

	static constexpr Timespan RECENT_WAR_LOSS_TIME_LIMIT = Timespan::from_years(5);

	/* Representation of a country's mutable attributes, with a CountryDefinition that is unique at any single time
	 * but can be swapped with other CountryInstance's CountryDefinition when switching tags. */
	struct CountryInstance : FlagStrings, HasIndex<CountryInstance, country_index_t>, PopsAggregate {
		friend struct CountryInstanceManager;

		/*
			Westernisation Progress vs Status for Uncivilised Countries:
				15 - primitive
				16 - uncivilised
				50 - uncivilised
				51 - partially westernised
		*/

		enum struct country_status_t : uint8_t {
			COUNTRY_STATUS_GREAT_POWER,
			COUNTRY_STATUS_SECONDARY_POWER,
			COUNTRY_STATUS_CIVILISED,
			COUNTRY_STATUS_PARTIALLY_CIVILISED,
			COUNTRY_STATUS_UNCIVILISED,
			COUNTRY_STATUS_PRIMITIVE
		};

		// The reserve of goods the government keeps in the national stockpile is set in the group ov_economy of the
		// defines; the pointer is set by the InstanceManager (the defines outlive all instances)
		static inline OpenVicEconomyDefines const* openvic_economy_defines = nullptr;
		static void set_openvic_economy_defines(OpenVicEconomyDefines const& defines) {
			openvic_economy_defines = &defines;
		}
		static inline OpenVicTradeDefines const* openvic_trade_defines = nullptr;
		static void set_openvic_trade_defines(OpenVicTradeDefines const& defines) {
			openvic_trade_defines = &defines;
		}
		static inline OpenVicSupplyDefines const* openvic_supply_defines = nullptr;
		static void set_openvic_supply_defines(OpenVicSupplyDefines const& defines) {
			openvic_supply_defines = &defines;
		}

		// Thresholds for different uncivilised country statuses
		static constexpr fixed_point_t PRIMITIVE_CIVILISATION_PROGRESS = fixed_point_t(15) / 100;
		static constexpr fixed_point_t UNCIVILISED_CIVILISATION_PROGRESS = fixed_point_t::_0_50;

	private:
		SharedCountryValues& shared_country_values;

		const Date fallback_date_for_never_completing_research;
		CountryDefines const& country_defines;
		DiplomacyDefines const& diplomacy_defines;
		EconomyDefines const& economy_defines;
		MilitaryDefines const& military_defines;

		GameRulesManager const& game_rules_manager;
		GoodInstanceManager& good_instance_manager;
		CountryRelationManager& country_relations_manager;
		MarketInstance& market_instance;
		ModifierEffectCache const& modifier_effect_cache;
		UnitTypeManager const& unit_type_manager;

		colour_t PROPERTY(colour); // Cached to avoid searching government overrides for every province
		ProvinceInstance* PROPERTY_PTR(capital, nullptr);
		bool PROPERTY_RW_CUSTOM_NAME(ai, is_ai, set_ai, true);
	public:
		// Who pays the losses of the subsidised factories: ALL (the state pays the whole loss), BY_SHARE (the state pays its
		// share of the ownership, the capitalists the rest), CRISIS (like BY_SHARE while the capitalists have money, when their
		// money runs out the state also pays their part, whatever its share is), HARD_CRISIS (like CRISIS, but every month
		// the state takes over the share of the factory that it paid for instead of the capitalists)
		enum struct subsidy_mode_t : uint32_t { ALL = 0, BY_SHARE = 1, CRISIS = 2, HARD_CRISIS = 3 };
	private:
		subsidy_mode_t PROPERTY_RW(subsidy_mode, subsidy_mode_t::BY_SHARE);
		// Central bank: yearly interest rates of the loans the country gives to its capitalists and to other countries
		fixed_point_t PROPERTY(internal_loan_rate, fixed_point_t(5) / 100);
		fixed_point_t PROPERTY(external_loan_rate, fixed_point_t(4) / 100);
	public:
		void set_central_bank_rates(const fixed_point_t internal_rate, const fixed_point_t external_rate) {
			internal_loan_rate = internal_rate;
			external_loan_rate = external_rate;
		}
	private:
		bool PROPERTY_CUSTOM_PREFIX(releasable_vassal, is, true);
		bool PROPERTY(owns_colonial_province, false);
		bool PROPERTY(has_unowned_cores, false);
		bool PROPERTY_CUSTOM_PREFIX(coastal, is, false);
		fixed_point_t PROPERTY(owned_cores_controlled_proportion);
		fixed_point_t PROPERTY(occupied_provinces_proportion);
		size_t PROPERTY(port_count, 0);

		country_status_t PROPERTY(country_status, country_status_t::COUNTRY_STATUS_UNCIVILISED);
		fixed_point_t PROPERTY(civilisation_progress);
		Date PROPERTY(lose_great_power_date);
		size_t PROPERTY(total_rank, 0);

		ordered_set<ProvinceInstance*> PROPERTY(owned_provinces);
		ordered_set<ProvinceInstance*> PROPERTY(controlled_provinces);
		ordered_set<ProvinceInstance*> PROPERTY(core_provinces);
		ordered_set<State*> PROPERTY(states);

		ordered_set<CountryInstance*> PROPERTY(neighbouring_countries);

		string_map_t<fixed_point_t> PROPERTY(script_variables);

		// The total/resultant modifier affecting this country, including owned province contributions.
		ModifierSum PROPERTY(modifier_sum);
		memory::vector<ModifierInstance> SPAN_PROPERTY(event_modifiers);

		/* Production */
		OV_STATE_PROPERTY(fixed_point_t, industrial_power);
		memory::vector<std::pair<std::reference_wrapper<const State>, fixed_point_t>> SPAN_PROPERTY(industrial_power_from_states);
		memory::vector<std::pair<std::reference_wrapper<const CountryInstance>, fixed_point_t>> SPAN_PROPERTY(industrial_power_from_investments);
		size_t PROPERTY(industrial_rank, 0);
		fixed_point_map_t<std::reference_wrapper<const CountryInstance>> PROPERTY(foreign_investments);
		OV_IFLATMAP_PROPERTY(BuildingType, building_level_t, building_type_unlock_levels);
		// TODO - total amount of each good produced

		/* Budget */
		fixed_point_t cash_stockpile_start_of_tick;
		ValueHistory<fixed_point_t> PROPERTY(balance_history);
		OV_STATE_PROPERTY(fixed_point_t, gold_income);
		atomic_fixed_point_t PROPERTY(cash_stockpile);
		fixed_point_t free_funds;
		spin_mutex taxable_income_mutex;
		OV_IFLATMAP_PROPERTY(PopType, fixed_point_t, taxable_income_by_pop_type);
		OV_STATE_PROPERTY(fixed_point_t, tax_efficiency);
		IndexedFlatMap<Strata, DerivedState<fixed_point_t>> PROPERTY(effective_tax_rate_by_strata);
	public:
		DerivedState<fixed_point_t>& get_effective_tax_rate_by_strata(Strata const& strata);
	private:
		IndexedFlatMap<Strata, ClampedValue> tax_rate_slider_value_by_strata;
	public:
		[[nodiscard]] constexpr IndexedFlatMap<Strata, ClampedValue> const& get_tax_rate_slider_value_by_strata() const {
			return tax_rate_slider_value_by_strata;
		}
		[[nodiscard]] ReadOnlyClampedValue& get_tax_rate_slider_value_by_strata(Strata const& strata);
		[[nodiscard]] ReadOnlyClampedValue const& get_tax_rate_slider_value_by_strata(Strata const& strata) const;
	private:

		OV_STATE_PROPERTY(fixed_point_t, administrative_efficiency_from_administrators);
		OV_STATE_PROPERTY(fixed_point_t, administrator_percentage);

		//store per slider per good: desired, bought & cost
		//store purchase record from last tick and prediction next tick
		OV_CLAMPED_PROPERTY(army_spending_slider_value);
		OV_CLAMPED_PROPERTY(navy_spending_slider_value);
		// The construction slider of the budget is the allocation to the investment fund: at 100 percent half of the income
		// of the day (see OpenVicEconomyDefines::investment_budget_share) goes into the fund, 1 percent of the slider is 0.5
		// percent of the budget. The fund is not limited. Once a week the InvestmentPlanner gives its money to the pops
		// who build the enterprises and railroads that would be profitable.
		OV_CLAMPED_PROPERTY(construction_spending_slider_value);
		fixed_point_t PROPERTY(investment_fund);
		fixed_point_t PROPERTY(investment_deposit_yesterday);
		fixed_point_t PROPERTY(investment_spent_total);
		// Money that came to the treasury yesterday (taxes, tariffs, gold, sales of the national stockpile): the budget
		fixed_point_t PROPERTY(income_yesterday);
		// Average (about a month) of the daily balance of the treasury before the deposit to the investment fund; the AI
		// sets its allocation from it so that the budget does not go into a loss
		fixed_point_t PROPERTY(balance_before_investment_average);
		std::int32_t balance_samples = 0;
		atomic_fixed_point_t actual_tax_income;
		atomic_fixed_point_t PROPERTY(actual_national_stockpile_spending);
		atomic_fixed_point_t PROPERTY(actual_national_stockpile_income);

		OV_CLAMPED_PROPERTY(administration_spending_slider_value);
		OV_STATE_PROPERTY(fixed_point_t, projected_administration_spending_unscaled_by_slider);
		fixed_point_t actual_administration_budget;
		bool PROPERTY(was_administration_budget_cut_yesterday, false);
		atomic_fixed_point_t PROPERTY(actual_administration_spending);

		OV_CLAMPED_PROPERTY(education_spending_slider_value);
		OV_STATE_PROPERTY(fixed_point_t, projected_education_spending_unscaled_by_slider);
		fixed_point_t actual_education_budget;
		bool PROPERTY(was_education_budget_cut_yesterday, false);
		atomic_fixed_point_t PROPERTY(actual_education_spending);

		OV_CLAMPED_PROPERTY(military_spending_slider_value);
		OV_STATE_PROPERTY(fixed_point_t, projected_military_spending_unscaled_by_slider);
		fixed_point_t actual_military_budget;
		bool PROPERTY(was_military_budget_cut_yesterday, false);
		atomic_fixed_point_t PROPERTY(actual_military_spending);

		OV_CLAMPED_PROPERTY(social_spending_slider_value);
		OV_STATE_PROPERTY(fixed_point_t, projected_pensions_spending_unscaled_by_slider);
		OV_STATE_PROPERTY(fixed_point_t, projected_unemployment_subsidies_spending_unscaled_by_slider);
		fixed_point_t actual_social_budget;
		bool PROPERTY(was_social_budget_cut_yesterday, false);
		atomic_fixed_point_t PROPERTY(actual_pensions_spending);
		atomic_fixed_point_t PROPERTY(actual_unemployment_subsidies_spending);

		//base here means not scaled by slider or pop size
		// The bases of the salaries and of the welfare of a type of pop: the costs of its needs (shared by all countries, they change with the
		// prices every day) times the corruption of the country. They are worked out when they are asked for: as reactive states (one per type
		// of pop and country, made again every day) they cost many times more than the product.
		fixed_point_t get_administration_salary_base(PopType const& pop_type);
		fixed_point_t get_education_salary_base(PopType const& pop_type);
		fixed_point_t get_military_salary_base(PopType const& pop_type);
		fixed_point_t get_social_income_variant_base(PopType const& pop_type);
		// The levels of the pensions and of the unemployment benefit of today (the modifiers do not change during a tick), set by country_tick_before_map
		fixed_point_t pension_level_today;
		fixed_point_t unemployment_benefit_today;

		OV_CLAMPED_PROPERTY(tariff_rate_slider_value);
		fixed_point_t actual_import_subsidies_budget;
		bool PROPERTY(was_import_subsidies_budget_cut_yesterday, false);
		atomic_fixed_point_t actual_import_subsidies_spending;
		atomic_fixed_point_t actual_tariff_income;
		// The freight the sellers of the country paid yesterday (inside the country and on the way abroad), see TradeNetwork
		atomic_fixed_point_t actual_transport_income;
		// The treasury of the last day by the sources that have no figure of their own (reset at the start of the tick, filled by
		// add_cash / spend_cash): used by the budget window
		atomic_fixed_point_t treasury_other_income;
		atomic_fixed_point_t treasury_other_spending;
		atomic_fixed_point_t treasury_subsidy_spending;
		fixed_point_t transport_to_treasury_last = 0;
		fixed_point_t get_actual_net_tariffs_balance() const {
			return actual_tariff_income.load() - actual_import_subsidies_spending.load();
		}

		//TODO actual factory subsidies
		//projected cost is UI only and lists the different factories

		/* Technology */
		OV_IFLATMAP_PROPERTY(Technology, technology_unlock_level_t, technology_unlock_levels);
		OV_IFLATMAP_PROPERTY(Invention, technology_unlock_level_t, invention_unlock_levels);
		OV_STATE_PROPERTY(int32_t, inventions_count);
		OV_STATE_PROPERTY(Technology const*, current_research, nullptr);
		OV_STATE_PROPERTY(fixed_point_t, invested_research_points);
		OV_STATE_PROPERTY(fixed_point_t, current_research_cost);
		OV_STATE_PROPERTY(Date, expected_research_completion_date);
		OV_STATE_PROPERTY(fixed_point_t, research_point_stockpile);
		OV_STATE_PROPERTY(fixed_point_t, daily_research_points);
		fixed_point_map_t<PopType const*> PROPERTY(research_points_from_pop_types);
		OV_STATE_PROPERTY(TechnologySchool const*, tech_school, nullptr);
		// TODO - cached possible inventions with %age chance

		/* Politics */
		OV_STATE_PROPERTY(NationalValue const*, national_value, nullptr);
		OV_STATE_PROPERTY(GovernmentType const*, government_type, nullptr);
		Date PROPERTY(last_election);
		OV_STATE_PROPERTY(CountryParty const*, ruling_party, nullptr);
		OV_IFLATMAP_PROPERTY(Ideology, fixed_point_t, upper_house_proportion_by_ideology);
		OV_IFLATMAP_PROPERTY(ReformGroup, Reform const*, reforms);
		OV_STATE_PROPERTY(fixed_point_t, total_administrative_multiplier);
		RuleSet PROPERTY(rule_set);
		// TODO - national issue support distribution (for just voters and for everyone)
		OV_IFLATMAP_PROPERTY(GovernmentType, GovernmentType const*, flag_overrides_by_government_type);
		OV_STATE_PROPERTY(fixed_point_t, suppression_points);
		OV_STATE_PROPERTY(fixed_point_t, infamy); // in 0-25+ range
		OV_STATE_PROPERTY(fixed_point_t, plurality); // in 0-100 range
		OV_STATE_PROPERTY(fixed_point_t, revanchism);
		OV_IFLATMAP_PROPERTY(Crime, technology_unlock_level_t, crime_unlock_levels);
		// TODO - rebel movements

		/* Population */
		size_t PROPERTY(national_focus_capacity, 0);
		Culture const* PROPERTY(primary_culture, nullptr);
		ordered_set<Culture const*> PROPERTY(accepted_cultures);
		Religion const* PROPERTY(religion, nullptr);
		// TODO - population change over last 30 days

	public:
		static constexpr size_t VECTORS_FOR_COUNTRY_TICK = 4;

		CountryDefinition const& country_definition;
		DerivedState<GovernmentType const*> flag_government_type;
		DerivedState<fixed_point_t> total_score;
		DerivedState<fixed_point_t> military_power;
		DerivedState<fixed_point_t> research_progress;
		DerivedState<fixed_point_t> desired_administrator_percentage;
		DerivedState<fixed_point_t> corruption_cost_multiplier;
		DerivedState<fixed_point_t> tariff_efficiency;
		DerivedState<fixed_point_t> effective_tariff_rate;
		DerivedState<fixed_point_t> projected_administration_spending;
		DerivedState<fixed_point_t> projected_education_spending;
		DerivedState<fixed_point_t> projected_military_spending;
		DerivedState<fixed_point_t> projected_social_spending;
		DerivedState<fixed_point_t> projected_social_spending_unscaled_by_slider;
		DerivedState<fixed_point_t> projected_import_subsidies;
		DerivedState<fixed_point_t> projected_spending;
		DerivedState<bool> has_import_subsidies;

		fixed_point_t get_taxable_income_by_strata(Strata const& strata) const;
		// TODO - national foci

		/* Trade */
		struct good_data_t {
			memory::unique_ptr<spin_mutex> mutex;
			fixed_point_t stockpile_amount;
			fixed_point_t stockpile_change_yesterday; // positive if we gained, negative if we lost
			fixed_point_t quantity_traded_yesterday; // positive if we bought, negative if we sold
			fixed_point_t money_traded_yesterday; // positive if we sold, negative if we bought

			bool is_automated = true;
			bool is_selling = false; // buying if false (manual only)
			fixed_point_t stockpile_cutoff; // (manual only)

			fixed_point_t exported_amount; // negative if net importing

			fixed_point_t max_government_consumption; //for automated BuyUpToOrder
			fixed_point_t government_needs; //quantity to allocate for while automated
			// The average supplies of the units per day (what the stockpile is kept ready for) ...
			fixed_point_t army_needs;
			fixed_point_t navy_needs;
			// ... and what the units whose supply day is today really use
			fixed_point_t army_due;
			fixed_point_t navy_due;
			fixed_point_t overseas_maintenance;
			fixed_point_t factory_demand;
			fixed_point_t pop_demand;
			fixed_point_t available_amount;

			ordered_map<PopType const*, fixed_point_t> need_consumption_per_pop_type;
			// The consumption of the pops is added up by the index of the type of pop while the day is played (millions of reports a day are
			// too many for a map) and copied into need_consumption_per_pop_type when the day is over (publish_need_consumption); the vector
			// only has numbers if there was a report today
			memory::vector<fixed_point_t> need_consumption_by_pop_type_index;
			bool has_need_consumption_reports = false;
			ordered_map<ProductionType const*, fixed_point_t> input_consumption_per_production_type;
			ordered_map<ProductionType const*, fixed_point_t> production_per_production_type;

			good_data_t();
			good_data_t(good_data_t&&) = default;
			good_data_t& operator=(good_data_t&&) = default;

			//thread safe
			void clear_daily_recorded_data();
		};

	private:
		OV_IFLATMAP_PROPERTY(GoodInstance, good_data_t, goods_data);

		/* Diplomacy */
		OV_STATE_PROPERTY(fixed_point_t, prestige);
	public:
		void add_prestige(const fixed_point_t delta) {
			prestige += delta;
		}
		void set_last_election(const Date date) {
			last_election = date;
		}
		void set_upper_house_proportion(Ideology const& ideology, fixed_point_t value);
		// Tariffs collected / import subsidies paid yesterday
		fixed_point_t get_tariff_income() const {
			return actual_tariff_income.load();
		}
		// Thread safe: the market adds the freight of the sellers of the country
		void add_transport_fee(const fixed_point_t amount) {
			actual_transport_income += amount;
		}
		fixed_point_t get_transport_income() const {
			return actual_transport_income.load();
		}
		fixed_point_t get_import_subsidies_spending() const {
			return actual_import_subsidies_spending.load();
		}
	private:
		size_t PROPERTY(prestige_rank, 0);
		fixed_point_t PROPERTY(diplomatic_points);
		// The last time this country lost a war, i.e. accepted a peace offer sent from their offer tab or the enemy's demand
		// tab, even white peace. Used for the "has_recently_lost_war" condition (true if the date is less than 5 years ago).
		Date PROPERTY(last_war_loss_date);
	public:
		void set_last_war_loss_date(const Date date) {
			last_war_loss_date = date;
		}
	private:
		vector_ordered_set<std::reference_wrapper<const CountryInstance>> PROPERTY(war_enemies);
		OV_STATE_PROPERTY(CountryInstance const*, sphere_owner, nullptr);
		// TODO - colonial power, current wars

		/* Military */
		OV_STATE_PROPERTY(fixed_point_t, military_power_from_land);
		OV_STATE_PROPERTY(fixed_point_t, military_power_from_sea);
		OV_STATE_PROPERTY(fixed_point_t, military_power_from_leaders);
		size_t PROPERTY(military_rank, 0);
		memory::vector<std::reference_wrapper<LeaderInstance>> SPAN_PROPERTY(generals);
		memory::vector<std::reference_wrapper<LeaderInstance>> SPAN_PROPERTY(admirals);
		memory::vector<std::reference_wrapper<ArmyInstance>> SPAN_PROPERTY(armies);
		memory::vector<std::reference_wrapper<NavyInstance>> SPAN_PROPERTY(navies);
		size_t PROPERTY(regiment_count, 0);
		size_t PROPERTY(mobilisation_potential_regiment_count, 0);
		size_t PROPERTY(mobilisation_max_regiment_count, 0);
		fixed_point_t PROPERTY(mobilisation_impact);
		fixed_point_t PROPERTY(mobilisation_economy_impact);
		fixed_point_t PROPERTY(supply_consumption);
		// The parts of the supply limit of the provinces that depend on the country of the army (docs/PLAN_ARMY_SUPPLY_FRONTS.md,
		// section 1), worked out every day: the global supply_limit that is added to the limit of the province, the multiplier of the
		// technologies and the bonus of the own land
		bool deep_assault_technology = false;
		// The templates of the divisions of the country (military/Division.hpp); the standard one is made the first time the country has the
		// technology of the organisation of the army
		memory::vector<DivisionTemplate> division_templates;
		std::uint32_t next_division_template_id = 1;
		bool division_templates_made = false;
		fixed_point_t PROPERTY(supply_limit_global_base);
		fixed_point_t PROPERTY(supply_limit_multiplier, fixed_point_t::_1);
		fixed_point_t PROPERTY(supply_home_bonus);
		size_t PROPERTY(ship_count, 0);
		fixed_point_t PROPERTY(total_consumed_ship_supply);
		fixed_point_t PROPERTY(max_ship_supply);
		fixed_point_t PROPERTY_RW(leadership_point_stockpile);
		fixed_point_t PROPERTY(monthly_leadership_points);
		fixed_point_map_t<PopType const*> PROPERTY(leadership_points_from_pop_types);
		int32_t PROPERTY(create_leader_count, 0);
		// War exhaustion is stored as a raw decimal rather than a proportion, it is usually in the range 0-100.
		// The current war exhaustion value should only ever be modified via change_war_exhaustion(delta) which also
		// clamps the result in the range [0, war_exhaustion_max] straight away, matching the base game's behaviour.
		fixed_point_t PROPERTY(war_exhaustion);
		// Share of the supplies the armies needed yesterday that the national stockpile could give (1 = fully supplied)
		fixed_point_t PROPERTY(army_supply_ratio, fixed_point_t::_1);
		// The priority of the supply of the armies that have no priority of their own (the setting of the tab of the armies)
		supply_priority_t PROPERTY_RW(army_supply_priority, supply_priority_t::STANDARD);
		fixed_point_t PROPERTY(navy_supply_ratio, fixed_point_t::_1);
		// How well equipped the army and the navy are (1 = fully): it goes to (the budget slider times the supply ratio) by a share a day
		// (ov_economy SUPPLY_LEVEL_CHANGE_PER_DAY); the attack, the defence and the gun power in battles are multiplied by it
		fixed_point_t PROPERTY(army_supply_level, fixed_point_t::_1);
		fixed_point_t PROPERTY(navy_supply_level, fixed_point_t::_1);
		fixed_point_t PROPERTY(war_exhaustion_max);
		bool PROPERTY_RW_CUSTOM_NAME(mobilised, is_mobilised, set_mobilised, false);
		bool PROPERTY_CUSTOM_PREFIX(disarmed, is, false);
		bool PROPERTY_RW(auto_create_leaders, true);
		bool PROPERTY_RW(auto_assign_leaders, true);
		fixed_point_t PROPERTY(organisation_regain);
		fixed_point_t PROPERTY(land_organisation);
		fixed_point_t PROPERTY(naval_organisation);
		fixed_point_t PROPERTY(land_unit_start_experience);
		fixed_point_t PROPERTY(naval_unit_start_experience);
		fixed_point_t PROPERTY(recruit_time);
		combat_width_t PROPERTY(combat_width, combat_width_t(1));
		int32_t PROPERTY(dig_in_cap, 0);
		fixed_point_t PROPERTY(military_tactics);
		memory::FixedVector<technology_unlock_level_t, regiment_type_index_t> PROPERTY(regiment_type_unlock_levels);
		regiment_allowed_cultures_t PROPERTY(allowed_regiment_cultures, regiment_allowed_cultures_t::NO_CULTURES);
		memory::FixedVector<technology_unlock_level_t, ship_type_index_t> PROPERTY(ship_type_unlock_levels);
		technology_unlock_level_t PROPERTY(gas_attack_unlock_level, technology_unlock_level_t { 0 });
		technology_unlock_level_t PROPERTY(gas_defence_unlock_level, technology_unlock_level_t { 0 });
		memory::vector<technology_unlock_level_t> SPAN_PROPERTY(unit_variant_unlock_levels);

	public:
		CountryInstance(
			CountryDefinition const& new_country_definition,
			SharedCountryValues& new_shared_country_values,
			CountryInstanceDeps const& country_instance_deps
		);
		CountryInstance(CountryInstance const&) = delete;
		CountryInstance& operator=(CountryInstance const&) = delete;
		CountryInstance(CountryInstance&&) = delete;
		CountryInstance& operator=(CountryInstance&&) = delete;

		OV_UNIT_BRANCHED_GETTER(get_unit_instance_groups, armies, navies);
		OV_UNIT_BRANCHED_GETTER(get_unit_type_unlock_levels, regiment_type_unlock_levels, ship_type_unlock_levels);
		OV_UNIT_BRANCHED_GETTER(get_leaders, generals, admirals);
		OV_UNIT_BRANCHED_GETTER_CONST(get_leaders, generals, admirals);

		[[nodiscard]] inline size_t get_general_count() const {
			return generals.size();
		}
		[[nodiscard]] inline bool has_generals() const {
			return !generals.empty();
		}
		[[nodiscard]] inline size_t get_admiral_count() const {
			return admirals.size();
		}
		[[nodiscard]] inline bool has_admirals() const {
			return !admirals.empty();
		}
		[[nodiscard]] inline size_t get_leader_count() const {
			return get_general_count() + get_admiral_count();
		}
		[[nodiscard]] inline bool has_leaders() const {
			return has_generals() || has_admirals();
		}
		[[nodiscard]] inline size_t get_army_count() const {
			return armies.size();
		}
		[[nodiscard]] inline bool has_armies() const {
			return !armies.empty();
		}
		[[nodiscard]] inline size_t get_navy_count() const {
			return navies.size();
		}
		[[nodiscard]] inline bool has_navies() const {
			return !navies.empty();
		}

		[[nodiscard]] std::string_view get_identifier() const;

		[[nodiscard]] bool exists() const;
		[[nodiscard]] bool is_rebel_country() const;
		[[nodiscard]] bool is_civilised() const;
		[[nodiscard]] bool can_colonise() const;
		[[nodiscard]] bool is_great_power() const;
		[[nodiscard]] bool is_secondary_power() const;
		[[nodiscard]] bool is_at_war() const;
		[[nodiscard]] bool is_neighbour(CountryInstance const& country) const;
		[[nodiscard]] bool may_build_in(const BuildingRestrictionCategory restriction_category, ProvinceInstance const& location) const;

		// Double-sided diplomacy functions

		CountryRelationManager::relation_value_type get_relations_with(CountryInstance const& country) const;
		void set_relations_with(CountryInstance& country, CountryRelationManager::relation_value_type relations);

		[[nodiscard]] bool has_alliance_with(CountryInstance const& country) const;
		void set_alliance_with(CountryInstance& country, bool alliance = true);

		[[nodiscard]] bool is_at_war_with(CountryInstance const& country) const;
		// Low-level setter function, should not be used to declare or join wars
		// Should generally be the basis for higher-level war functions
		void set_at_war_with(CountryInstance& country, bool at_war = true);

		// Single-sided diplomacy functions

		// Only detects military access diplomacy, do not use to validate troop movement
		// Prefer can_units_enter
		[[nodiscard]] bool has_military_access_to(CountryInstance const& country) const;
		void set_military_access_to(CountryInstance& country, bool access = true);

		[[nodiscard]] bool is_sending_war_subsidy_to(CountryInstance const& country) const;
		void set_sending_war_subsidy_to(CountryInstance& country, bool sending = true);

		[[nodiscard]] bool is_commanding_units(CountryInstance const& country) const;
		void set_commanding_units(CountryInstance& country, bool commanding = true);

		[[nodiscard]] bool has_vision_of(CountryInstance const& country) const;
		void set_has_vision_of(CountryInstance& country, bool vision = true);

		CountryRelationManager::OpinionType get_opinion_of(CountryInstance const& country) const;
		void set_opinion_of(CountryInstance& country, CountryRelationManager::OpinionType opinion);
		void increase_opinion_of(CountryInstance& country);
		void decrease_opinion_of(CountryInstance& country);

		CountryRelationManager::influence_value_type get_influence_with(CountryInstance const& country) const;
		void set_influence_with(CountryInstance& country, CountryRelationManager::influence_value_type influence);

		CountryRelationManager::influence_priority_value_type get_influence_priority_with(CountryInstance const& country) const;
		void set_influence_priority_with(CountryInstance& country, CountryRelationManager::influence_priority_value_type influence);

		std::optional<Date> get_decredited_from_date(CountryInstance const& country) const;
		void set_discredited_from(CountryInstance& country, Date until);

		std::optional<Date> get_embass_banned_from_date(CountryInstance const& country) const;
		void set_embassy_banned_from(CountryInstance& country, Date until);

		[[nodiscard]] bool can_army_units_enter(CountryInstance const& country) const;
		[[nodiscard]] bool can_navy_units_enter(CountryInstance const& country) const;

		// These functions take "std::string const&" rather than "std::string_view" as they're only used with script arguments
		// which are always stored as "std::string"s and it significantly simplifies mutable value access.
		fixed_point_t get_script_variable(memory::string const& variable_name) const;
		void set_script_variable(memory::string const& variable_name, fixed_point_t value);
		// Adds the argument value to the existing value of the script variable (initialised to 0 if it doesn't already exist).
		void change_script_variable(memory::string const& variable_name, fixed_point_t value);

		bool add_owned_province(ProvinceInstance& new_province);
		bool remove_owned_province(ProvinceInstance const& province_to_remove);

		bool add_controlled_province(ProvinceInstance& new_province);
		bool remove_controlled_province(ProvinceInstance const& province_to_remove);

		bool add_core_province(ProvinceInstance& new_core);
		bool remove_core_province(ProvinceInstance const& core_to_remove);

		bool add_state(State& new_state);
		bool remove_state(State const& state_to_remove);

		bool add_accepted_culture(Culture const& new_accepted_culture);
		bool remove_accepted_culture(Culture const& culture_to_remove);

		bool set_ruling_party(CountryParty const& new_ruling_party);
		bool add_reform(Reform const& new_reform);

		void set_strata_tax_rate_slider_value(Strata const& strata, const fixed_point_t new_value);
		// The last projects paid from the investment fund (newest last), for the interface
		struct InvestmentRecord {
			enum struct kind_t : std::uint8_t { NEW_FACTORY, EXPANDED_FACTORY, RAILROAD };
			Date date;
			kind_t kind = kind_t::NEW_FACTORY;
			BuildingType const* building_type = nullptr;
			// The state of the factory, or the state of the province of the railroad
			State const* state = nullptr;
			ProvinceInstance const* province = nullptr;
			PopType const* builder = nullptr;
			fixed_point_t cost;
			// The project was chosen first because it makes a good that the country lacks
			bool deficit = false;
		};
		static constexpr size_t MAX_INVESTMENT_RECORDS = 16;
	private:
		memory::vector<InvestmentRecord> recent_investments;
	public:
		[[nodiscard]] memory::vector<InvestmentRecord> const& get_recent_investments() const {
			return recent_investments;
		}
		// Pays a project out of the investment fund (the money is spent on the construction and leaves the economy)
		void spend_investment_fund(const fixed_point_t amount, InvestmentRecord&& record) {
			investment_fund -= amount;
			investment_spent_total += amount;
			MoneyLedger::record(MoneyLedger::label_t::investment_fund_spent, -amount);
			if (recent_investments.size() >= MAX_INVESTMENT_RECORDS) {
				recent_investments.erase(recent_investments.begin());
			}
			recent_investments.push_back(std::move(record));
		}
		// What the construction slider sets aside per day for the given value of the slider (when it is deposited it cannot be more
		// than the money in the treasury)
		[[nodiscard]] fixed_point_t get_investment_allocation(fixed_point_t slider_value) const;
		[[nodiscard]] fixed_point_t get_investment_allocation() const {
			return get_investment_allocation(construction_spending_slider_value.get_value_untracked());
		}
		// The share of the income that 100 percent of the construction slider sets aside (50 percent)
		[[nodiscard]] static fixed_point_t get_investment_budget_share() {
			return openvic_economy_defines->investment_budget_share.fixed();
		}
		// Puts money into the national stockpile, e.g. the dividends of state owned factories
		void add_cash(const fixed_point_t amount) {
			cash_stockpile += amount;
			treasury_other_income += amount;
			MoneyLedger::record(MoneyLedger::label_t::country_other_credit, amount);
			free_funds += amount;
		}
		// Takes money out of the national stockpile, e.g. to pay for construction
		void spend_cash(const fixed_point_t amount) {
			cash_stockpile -= amount;
			treasury_other_spending += amount;
			MoneyLedger::record(MoneyLedger::label_t::country_other_debit, -amount);
			free_funds -= amount;
		}
		// The same, but the money covers the losses of the subsidised factories
		void spend_cash_for_subsidy(const fixed_point_t amount) {
			cash_stockpile -= amount;
			treasury_subsidy_spending += amount;
			MoneyLedger::record(MoneyLedger::label_t::country_other_debit, -amount);
			free_funds -= amount;
		}
		// Money of the last day that came to the treasury or left it by the sources that have no figure of their own
		[[nodiscard]] fixed_point_t get_treasury_other_income() const {
			return treasury_other_income.load();
		}
		[[nodiscard]] fixed_point_t get_treasury_other_spending() const {
			return treasury_other_spending.load();
		}
		[[nodiscard]] fixed_point_t get_treasury_subsidy_spending() const {
			return treasury_subsidy_spending.load();
		}
		// The part of the freight of the last day that went to the treasury (the rest went to the investment fund)
		[[nodiscard]] fixed_point_t get_transport_to_treasury() const {
			return transport_to_treasury_last;
		}
		// What decides how many supplies a group uses: the multiplier of the country (research and the like, on enemy land), whether
		// the group moves, whether it stands in a port (a fleet on land)
		struct supply_state_t {
			fixed_point_t multiplier;
			bool moving;
			bool in_port;
		};
		[[nodiscard]] supply_state_t get_supply_state(UnitInstanceGroup const& group) const;
		// TODO research: the technologies and inventions change here how many supplies the units use (the shares themselves are
		// in the defines ARMY_SUPPLY_* and NAVY_SUPPLY_*; the Vic2 modifier supply_consumption is not used because the static
		// modifier "war" of Vic2 halves it)
		[[nodiscard]] fixed_point_t get_supply_research_multiplier() const {
			return fixed_point_t::_1;
		}
		// TODO research: the technologies that raise the supply limit of the provinces (as a multiplier) and the bonus of the
		// own land (SUPPLY_HOME_BONUS_TECH, added to SUPPLY_HOME_BONUS when the technology is known)
		[[nodiscard]] fixed_point_t get_supply_limit_research_multiplier() const {
			return fixed_point_t::_1;
		}
		[[nodiscard]] bool has_supply_home_bonus_technology() const {
			return false;
		}
		// TODO research: the technologies that change the supply coefficient of the armies away from the own land (SUPPLY_NEIGHBOUR_ONE_TECH:
		// one held neighbour gives 100 percent; SUPPLY_FAR_TECH: more far away; "modern armies" opens the search through two provinces)
		// The technology of the organisation of the army opens the divisions, the templates, the generals and the reserve of supplies of the
		// regiments (a week); only the civilised countries can have it. Now it is a flag that the civilised countries have from the start.
		[[nodiscard]] bool has_army_organisation() const {
			return is_civilised();
		}
		// The technology of the deep strikes and assaults: better supply on the land of an enemy (one held neighbour gives 100 percent, a bonus
		// to the supply far away), the supply through two provinces and one more week of the reserve of the regiments. Not in the game yet.
		[[nodiscard]] bool has_deep_assault_technology() const {
			return deep_assault_technology;
		}
		// A technology of the late game that adds one more day to the reserve of the regiments (not in the game yet)
		[[nodiscard]] bool has_late_supply_reserve_technology() const {
			return false;
		}
		void set_deep_assault_technology(const bool known) {
			deep_assault_technology = known;
		}
		[[nodiscard]] bool has_supply_neighbour_technology() const {
			return has_deep_assault_technology();
		}
		[[nodiscard]] bool has_supply_far_technology() const {
			return has_deep_assault_technology();
		}
		[[nodiscard]] bool has_supply_gap2_technology() const {
			return has_deep_assault_technology();
		}
		// The priority that applies to an army (its own or the one of the country)
		[[nodiscard]] supply_priority_t get_effective_supply_priority(
			bool army_has_own_priority, supply_priority_t army_priority
		) const {
			return army_has_own_priority ? army_priority : army_supply_priority;
		}
		[[nodiscard]] std::span<const DivisionTemplate> get_division_templates() const {
			return division_templates;
		}
		[[nodiscard]] DivisionTemplate const* find_division_template(const std::uint32_t id) const {
			for (DivisionTemplate const& division_template : division_templates) {
				if (division_template.id == id) {
					return &division_template;
				}
			}
			return nullptr;
		}
		// For the commands and the daily update only
		memory::vector<DivisionTemplate>& get_mutable_division_templates() {
			return division_templates;
		}
		std::uint32_t take_division_template_id() {
			return next_division_template_id++;
		}
		bool are_division_templates_made() const {
			return division_templates_made;
		}
		void set_division_templates_made(const bool made) {
			division_templates_made = made;
		}
		// Days of the reserve of supplies a regiment of this country keeps
		[[nodiscard]] fixed_point_t get_supply_reserve_capacity() const;
		// What the supplies of the army and the navy cost per day at the prices of the market (at 100 percent of the slider)
		[[nodiscard]] fixed_point_t calculate_army_supply_cost_per_day() const;
		[[nodiscard]] fixed_point_t calculate_navy_supply_cost_per_day() const;
		// The money that is not committed to the budget and the national stockpile purchases of the day: what can be
		// spent on subsidies, loans and the like while the day is being simulated without overspending the treasury
		fixed_point_t get_spendable_cash() const {
			const fixed_point_t cash = cash_stockpile.load();
			const fixed_point_t free = free_funds;
			const fixed_point_t spendable = cash < free ? cash : free;
			return spendable > fixed_point_t::_0 ? spendable : fixed_point_t::_0;
		}
		void set_army_spending_slider_value(const fixed_point_t new_value);
		void set_navy_spending_slider_value(const fixed_point_t new_value);
		void set_construction_spending_slider_value(const fixed_point_t new_value);
		void set_education_spending_slider_value(const fixed_point_t new_value);
		void set_administration_spending_slider_value(const fixed_point_t new_value);
		void set_social_spending_slider_value(const fixed_point_t new_value);
		void set_military_spending_slider_value(const fixed_point_t new_value);
		void set_tariff_rate_slider_value(const fixed_point_t new_value);

		// Adds delta to the current war exhaustion value and clamps it to the range [0, war_exhaustion_max].
		void change_war_exhaustion(fixed_point_t delta);

		// Changes the capital (used when the old one is lost in a peace deal)
		void set_capital(ProvinceInstance* new_capital) {
			capital = new_capital;
		}

		bool add_unit_instance_group(UnitInstanceGroup& group);
		bool remove_unit_instance_group(UnitInstanceGroup const& group);

		bool add_leader(LeaderInstance& leader);
		bool remove_leader(LeaderInstance const& leader);

		[[nodiscard]] bool has_leader_with_name(std::string_view name) const;

		[[nodiscard]] constexpr bool is_unit_type_unlocked(const regiment_type_index_t regiment_type_index) const {
			return is_unlocked(regiment_type_unlock_levels[regiment_type_index]);
		}
		[[nodiscard]] constexpr bool is_unit_type_unlocked(const ship_type_index_t ship_type_index) const {
			return is_unlocked(ship_type_unlock_levels[ship_type_index]);
		}
		[[nodiscard]] bool is_unit_type_unlocked(UnitType const& unit_type) const;

		bool modify_unit_type_unlock(UnitType const&, technology_unlock_level_t unlock_level_change);
		bool modify_unit_type_unlock(const regiment_type_index_t regiment_type_index, technology_unlock_level_t unlock_level_change);
		bool modify_unit_type_unlock(const ship_type_index_t ship_type_index, technology_unlock_level_t unlock_level_change);
		bool unlock_unit_type(UnitType const& unit_type);

		bool modify_building_type_unlock(
			BuildingType const& building_type, technology_unlock_level_t unlock_level_change
		);
		bool unlock_building_type(BuildingType const& building_type);
		[[nodiscard]] bool is_building_type_unlocked(BuildingType const& building_type) const;

		bool modify_crime_unlock(Crime const& crime, technology_unlock_level_t unlock_level_change);
		bool unlock_crime(Crime const& crime);
		[[nodiscard]] bool is_crime_unlocked(Crime const& crime) const;

		bool modify_gas_attack_unlock(technology_unlock_level_t unlock_level_change);
		bool unlock_gas_attack();
		[[nodiscard]] bool is_gas_attack_unlocked() const;

		bool modify_gas_defence_unlock(technology_unlock_level_t unlock_level_change);
		bool unlock_gas_defence();
		[[nodiscard]] bool is_gas_defence_unlocked() const;

		bool modify_unit_variant_unlock(unit_variant_t unit_variant, technology_unlock_level_t unlock_level_change);
		bool unlock_unit_variant(unit_variant_t unit_variant);
		[[nodiscard]] unit_variant_t get_max_unlocked_unit_variant() const;

		bool modify_technology_unlock(
			Technology const& technology, technology_unlock_level_t unlock_level_change
		);
		bool set_technology_unlock_level(
			Technology const& technology, technology_unlock_level_t unlock_level
		);
		bool unlock_technology(Technology const& technology);
		[[nodiscard]] bool is_technology_unlocked(Technology const& technology) const;

		bool modify_invention_unlock(
			Invention const& invention, technology_unlock_level_t unlock_level_change
		);
		bool set_invention_unlock_level(
			Invention const& invention, technology_unlock_level_t unlock_level
		);
		bool unlock_invention(Invention const& invention);
		[[nodiscard]] bool is_invention_unlocked(Invention const& invention) const;

		[[nodiscard]] bool is_primary_culture(Culture const& culture) const;
		// This only checks the accepted cultures list, ignoring the primary culture.
		[[nodiscard]] bool is_accepted_culture(Culture const& culture) const;
		[[nodiscard]] bool is_primary_or_accepted_culture(Culture const& culture) const;

		[[nodiscard]] fixed_point_t calculate_research_cost(Technology const& technology) const;
		[[nodiscard]] bool can_research_tech(Technology const& technology, const Date today) const;
		void start_research(Technology const& technology, const Date today);

		// Sets the investment of each country in the map (rather than adding to them), leaving the rest unchanged.
		void apply_foreign_investments(
			fixed_point_map_t<CountryDefinition const*> const& investments,
			CountryInstanceManager const& country_instance_manager
		);

		bool apply_history_to_country(
			CountryHistoryEntry const& entry,
			CountryInstanceManager const& country_instance_manager,
			FlagStrings& global_flags,
			MapInstance& map_instance
		);

	private:
		OV_ALWAYS_INLINE static constexpr bool is_unlocked(const technology_unlock_level_t unlock_level) {
			return unlock_level > 0;
		}

		template<typename IndexT>
		constexpr bool validate_unit_unlock_change(
			const IndexT index,
			const technology_unlock_level_t unlock_level,
			const technology_unlock_level_t unlock_level_change
		) const {
			// This catches subtracting below 0 or adding above the int types maximum value
			if (unlock_level + unlock_level_change < 0) {
				spdlog::error_s(
					"Attempted to change unlock level for {} in country {} to invalid value: current level = {}, change = {}, invalid new value = {}",
					index, *this, unlock_level,
					unlock_level_change, unlock_level + unlock_level_change
				);
				return false;
			}

			return true;
		}

		static void after_buy(void* actor, BuyResult const& buy_result);
		//matching GoodMarketSellOrder::callback_t
		static void after_sell(void* actor, SellResult const& sell_result, memory::vector<fixed_point_t>& reusable_vector);

		void calculate_government_good_needs(const Date today);

		void manage_national_stockpile(
			TypedSpan<good_index_t, char> reusable_goods_mask,
			forwardable_span<
				memory::vector<fixed_point_t>,
				VECTORS_FOR_COUNTRY_TICK
			> reusable_vectors,
			memory::vector<good_index_t>& reusable_good_index_vector,
			fixed_point_t& available_funds
		);

		void _update_production();
		void _update_budget();

		//base here means not scaled by slider or pop size
		fixed_point_t calculate_pensions_base(PopType const& pop_type);
		fixed_point_t calculate_unemployment_subsidies_base(PopType const& pop_type);

		// Expects current_research to be non-null
		void _update_current_tech(const Date today);
		void _update_technology(const Date today);
		void _update_politics();
		void _update_population();
		void _update_diplomacy();
		void _update_military();

		bool update_rule_set();

	public:
		void update_modifier_sum(Date today, StaticModifierCache const& static_modifier_cache);
		void make_room_for_province_modifier_sum(ModifierSum const& province_modifier_sum);
		void contribute_province_modifier_sum(ModifierSum const& province_modifier_sum);
		fixed_point_t get_modifier_effect_value(ModifierEffect const& effect) const;
		constexpr void for_each_contributing_modifier(
			ModifierEffect const& effect, ContributingModifierCallback auto callback
		) const {
			return modifier_sum.for_each_contributing_modifier(effect, std::move(callback));
		}

		void update_gamestate(const Date today, MapInstance& map_instance);
		void country_tick_before_map(
			const Date today,
			TypedSpan<good_index_t, char> reusable_goods_mask,
			forwardable_span<
				memory::vector<fixed_point_t>,
				VECTORS_FOR_COUNTRY_TICK
			> reusable_vectors,
			memory::vector<good_index_t>& reusable_good_index_vector
		);
		void country_tick_after_map(const Date today);

		good_data_t& get_good_data(GoodInstance const& good_instance);
		good_data_t const& get_good_data(GoodInstance const& good_instance) const;
		good_data_t& get_good_data(GoodDefinition const& good_definition);
		good_data_t const& get_good_data(GoodDefinition const& good_definition) const;

		//thread safe
		void report_pop_income_tax(PopType const& pop_type, const fixed_point_t gross_income, const fixed_point_t paid_as_tax);
		void report_pop_need_consumption(PopType const& pop_type, const good_index_t good_index, const fixed_point_t quantity);
		// Copies the consumption that was reported during the day into good_data_t::need_consumption_per_pop_type (done by country_tick_after_map)
		void publish_need_consumption();
		// For the self tests and the headless option -V: the consumption is also added up in need_consumption_per_pop_type while the day is
		// played (the way it was done before the flat sums) and compared with the flat sums when the day is over; a difference is logged and
		// counted
		static void set_verify_need_consumption(const bool verify) {
			verify_need_consumption = verify;
		}
		static std::size_t get_need_consumption_mismatch_count() {
			return need_consumption_mismatch_count;
		}
	private:
		inline static std::atomic<bool> verify_need_consumption = false;
		inline static std::atomic<std::size_t> need_consumption_mismatch_count = 0;
	public:
		void report_pop_need_demand(PopType const& pop_type, const good_index_t good_index, const fixed_point_t quantity);
		// The same for the demand of many pops added up (the pop type does not matter for the demand)
		void report_pop_need_demand_total(const good_index_t good_index, const fixed_point_t quantity);
		void report_input_consumption(ProductionType const& production_type, const good_index_t good_index, const fixed_point_t quantity);
		void report_input_demand(ProductionType const& production_type, const good_index_t good_index, const fixed_point_t quantity);
		void report_output(ProductionType const& production_type, const fixed_point_t quantity);
		void request_salaries_and_welfare_and_import_subsidies(Pop& pop);
		fixed_point_t calculate_minimum_wage_base(PopType const& pop_type);
		fixed_point_t apply_tariff(const fixed_point_t money_spent_on_imports);
	};
}

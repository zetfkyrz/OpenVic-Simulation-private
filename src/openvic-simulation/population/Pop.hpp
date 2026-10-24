#pragma once

#include <cstddef>
#include <functional>

#include <type_safe/strong_typedef.hpp>

#include "openvic-simulation/core/memory/Vector.hpp"
#include "openvic-simulation/core/object/FixedPoint.hpp"
#include "openvic-simulation/core/object/FixedPoint/Atomic.hpp"
#include "openvic-simulation/core/portable/ForwardableSpan.hpp"
#include "openvic-simulation/economy/production/ArtisanalProducer.hpp"
#include "openvic-simulation/population/PopIdInProvince.hpp"
#include "openvic-simulation/population/PopNeedsMacro.hpp"
#include "openvic-simulation/population/PopSize.hpp"
#include "openvic-simulation/types/fixed_point/FixedPointMap.hpp"
#include "openvic-simulation/types/UnitBranchType.hpp"

namespace OpenVic {
	struct BuyResult;
	struct CountryInstance;
	struct CountryParty;
	struct Culture;
	struct MarketInstance;
	struct MilitaryDefines;
	struct PopDeps;
	struct PopManager;
	struct PopType;
	struct PopValuesFromProvince;
	struct ProvinceInstance;
	struct RebelType;
	struct Reform;
	struct Religion;
	struct SellResult;

	struct PopBase {
		friend PopManager;

	protected:
		std::reference_wrapper<const PopType> PROPERTY_ACCESS(type, protected);
		pop_size_t PROPERTY_ACCESS(size, protected);
		fixed_point_t PROPERTY_RW_ACCESS(militancy, protected);
		fixed_point_t PROPERTY_RW_ACCESS(consciousness, protected);
		RebelType const* PROPERTY_ACCESS(rebel_type, protected);

		PopBase(
			PopType const& new_type, Culture const& new_culture, Religion const& new_religion, pop_size_t new_size,
			fixed_point_t new_militancy, fixed_point_t new_consciousness, RebelType const* new_rebel_type
		);
	public:
		Culture const& culture;
		Religion const& religion;

		// A copy with another type, culture and size (the militancy and consciousness are kept), for pops that are created
		// by the changes of the population
		PopBase with_changes(PopType const& new_type, Culture const& new_culture, pop_size_t new_size) const {
			return { new_type, new_culture, religion, new_size, militancy, consciousness, rebel_type };
		}
	};

	#define OV_DO_FOR_ALL_TYPES_OF_POP_INCOME(F) \
		F(rgo_owner_income) \
		F(rgo_worker_income) \
		F(factory_worker_income) \
		F(factory_owner_income) \
		F(unemployment_subsidies) \
		F(pensions) \
		F(government_salary_administration) \
		F(government_salary_education) \
		F(government_salary_military) \
		F(event_and_decision_income) \
		F(loan_interest_payments)

	#define OV_DO_FOR_ALL_TYPES_OF_POP_EXPENSES(F) \
		F(life_needs_expense) \
		F(everyday_needs_expense) \
		F(luxury_needs_expense) \
		F(artisan_inputs_expense)

	/* REQUIREMENTS:
	 * POP-18, POP-19, POP-20, POP-21, POP-34, POP-35, POP-36, POP-37
	 */
	struct Pop : PopBase {
		enum struct culture_status_t : uint8_t {
			UNACCEPTED, ACCEPTED, PRIMARY
		};

		static constexpr bool is_culture_status_allowed(regiment_allowed_cultures_t allowed, culture_status_t status) {
			return static_cast<uint8_t>(allowed) <= static_cast<uint8_t>(status);
		}

		static constexpr pop_size_t MAX_SIZE = std::numeric_limits<type_safe::underlying_type<pop_size_t>>::max();

	public:
		const pop_id_in_province_t id_in_province;

	private:
		MarketInstance& market_instance;
		std::optional<ArtisanalProducer> artisanal_producer_optional;
		fixed_point_t cash_allocated_for_artisanal_spending = 0;
		pop_size_t employed = 0;

		const std::reference_wrapper<ProvinceInstance> PROPERTY(location);

	public:
		[[nodiscard]] constexpr ProvinceInstance& get_location() {
			return location;
		}

	private:

		/* Last day's size change by source. */
		pop_size_t PROPERTY(total_change, 0);
		pop_size_t PROPERTY(num_grown, 0);
		pop_size_t PROPERTY(num_promoted, 0); // TODO - detailed promotion/demotion info (what to)
		pop_size_t PROPERTY(num_demoted, 0);
		pop_size_t PROPERTY(num_migrated_internal, 0); // TODO - detailed migration info (where to)
		pop_size_t PROPERTY(num_migrated_external, 0);
		pop_size_t PROPERTY(num_migrated_colonial, 0);

		culture_status_t PROPERTY(culture_status, culture_status_t::UNACCEPTED);

		static constexpr fixed_point_t DEFAULT_POP_LITERACY = fixed_point_t::_0_10;
		fixed_point_t PROPERTY_RW(literacy, DEFAULT_POP_LITERACY);

		// All of these should have a total size equal to the pop size, allowing the distributions from different pops to be
		// added together with automatic weighting based on their relative sizes. Similarly, the province, state and country
		// equivalents of these distributions will have a total size equal to their total population size.
		memory::FixedVector<fixed_point_t, ideology_index_t> SPAN_PROPERTY(supporter_equivalents_by_ideology);
		memory::FixedVector<fixed_point_t, party_policy_index_t> SPAN_PROPERTY(supporter_equivalents_by_party_policy);
		memory::FixedVector<fixed_point_t, reform_index_t> SPAN_PROPERTY(supporter_equivalents_by_reform);
		fixed_point_map_t<CountryParty const*> PROPERTY(vote_equivalents_by_party);

	public:
		// The values returned by these functions are scaled by pop size, so they must be divided by pop size to get
		// the support as a proportion of 1.0
		fixed_point_t get_vote_equivalents_by_party(CountryParty const& party) const;

		static constexpr pop_size_t size_denominator = 200000;
		static constexpr size_t VECTORS_FOR_POP_TICK = 5;

		constexpr pop_size_t get_unemployed() const {
			return size - employed;
		}
		fixed_point_t get_unemployment_fraction() const;
	private:
		fixed_point_t PROPERTY(income);
		fixed_point_t PROPERTY(savings);
		moveable_atomic_fixed_point_t PROPERTY(cash);
		moveable_atomic_fixed_point_t PROPERTY(expenses); //positive value means POP paid for goods. This is displayed * -1 in UI.
		moveable_atomic_fixed_point_t PROPERTY(yesterdays_import_value);

		#define NEED_MEMBERS(need_category) \
			moveable_atomic_fixed_point_t need_category##_needs_acquired_quantity, need_category##_needs_desired_quantity; \
			public: \
				fixed_point_t get_##need_category##_needs_fulfilled() const; \
			private: \
				fixed_point_map_t<good_index_t> PROPERTY(need_category##_needs); /* TODO pool? (if recalculating in UI is acceptable) */ \
				ordered_map<good_index_t, bool> PROPERTY(need_category##_needs_fulfilled_goods);

		OV_DO_FOR_ALL_NEED_CATEGORIES(NEED_MEMBERS)
		#undef NEED_MEMBERS

		fixed_point_t PROPERTY(artisanal_revenue);
		#define DECLARE_POP_MONEY_STORES(money_type) \
			fixed_point_t PROPERTY(money_type);

		OV_DO_FOR_ALL_TYPES_OF_POP_INCOME(DECLARE_POP_MONEY_STORES);
		OV_DO_FOR_ALL_TYPES_OF_POP_EXPENSES(DECLARE_POP_MONEY_STORES);
		#undef DECLARE_POP_MONEY_STORES

		std::size_t PROPERTY(regiment_count, 0);
		std::size_t PROPERTY(max_supported_regiments, 0);

		memory::string get_pop_context_text() const;
		void reserve_needs_fulfilled_goods();
		void fill_needs_fulfilled_goods_with_false();
		void allocate_for_needs(
			fixed_point_map_t<good_index_t> const& scaled_needs,
			forwardable_span<fixed_point_t> money_to_spend_per_good,
			memory::vector<fixed_point_t>& reusable_vector,
			fixed_point_t& price_inverse_sum,
			fixed_point_t& cash_left_to_spend
		);
		void pop_tick_without_cleanup(
			PopValuesFromProvince const& shared_values,
			RandomU32& random_number_generator,
			TypedSpan<good_index_t, char> reusable_goods_mask,
			forwardable_span<
				memory::vector<fixed_point_t>,
				VECTORS_FOR_POP_TICK
			> reusable_vectors
		);
		void pay_income_tax(fixed_point_t& income);

		template<bool IsTaxable>
		void add_artisanal_revenue(const fixed_point_t revenue);

		static void after_buy(void* actor, BuyResult const& buy_result);
		//matching GoodMarketSellOrder::callback_t
		static void after_sell(void* actor, SellResult const& sell_result, memory::vector<fixed_point_t>& reusable_vector);

	public:
		Pop(
			ProvinceInstance& new_location,
			PopBase const& pop_base,
			PopDeps const& pop_deps,
			const pop_id_in_province_t new_id_in_province
		);
		Pop(Pop const&) = delete;
		Pop(Pop&&) = default;
		Pop& operator=(Pop const&) = delete;
		Pop& operator=(Pop&&) = delete;

		// The artisan business of the pop, nullptr if it is not an artisan (for the reports)
		ArtisanalProducer const* get_artisanal_producer() const {
			return artisanal_producer_optional.has_value() ? &artisanal_producer_optional.value() : nullptr;
		}

		// The savings of the pop at the start of the game (see InstanceManager::initialise_pop_savings)
		void set_start_cash(const fixed_point_t amount) {
			cash = amount;
		}

		void setup_pop_test_values(TypedSpan<reform_index_t, const Reform> reforms);
		bool convert_to_equivalent();
		void update_location_based_attributes();

		void update_gamestate(
			MilitaryDefines const& military_defines,
			CountryInstance const* owner,
			const fixed_point_t pop_size_per_regiment_multiplier
		);

		#define DECLARE_POP_MONEY_STORE_FUNCTIONS(name) \
			void add_##name(fixed_point_t amount);

		OV_DO_FOR_ALL_TYPES_OF_POP_INCOME(DECLARE_POP_MONEY_STORE_FUNCTIONS)
		OV_DO_FOR_ALL_TYPES_OF_POP_EXPENSES(DECLARE_POP_MONEY_STORE_FUNCTIONS)
		DECLARE_POP_MONEY_STORE_FUNCTIONS(import_subsidies)
		#undef DECLARE_POP_MONEY_STORE_FUNCTIONS

		void pop_tick(
			PopValuesFromProvince const& shared_values,
			RandomU32& random_number_generator,
			TypedSpan<good_index_t, char> reusable_goods_mask,
			forwardable_span<
				memory::vector<fixed_point_t>,
				VECTORS_FOR_POP_TICK
			> reusable_vectors
		);
		void allocate_cash_for_artisanal_spending(const fixed_point_t money_to_spend);
		// Takes at most the amount out of the cash of the pop (e.g. a debt that is collected), returns what was taken
		fixed_point_t take_cash(const fixed_point_t amount);
		void hire(pop_size_t count);


		// Political support computed by the politics module: all values are already scaled to the size of the pop
		void set_political_support(
			std::span<const fixed_point_t> ideology_support, std::span<const fixed_point_t> party_policy_support,
			std::span<const fixed_point_t> reform_support
		);
		void set_party_votes(fixed_point_map_t<CountryParty const*> const& votes);

		// Population dynamics (monthly): people join or leave the pop, the change is recorded for the interface.
		// The size never goes below zero. kind: 0 growth, 1 promotion (arrived by promotion), 2 demotion, 3 internal migration
		void change_size(std::int32_t delta, std::int32_t kind);
		// Fraction of a person that growth could not add yet
		fixed_point_t growth_remainder;
		fixed_point_t assimilation_remainder;
		fixed_point_t migration_remainder;
		void clear_dynamics_record();
		//recruit or conscript
		bool try_recruit();
		bool try_recruit_understrength();
		// Frees a regiment slot when a regiment backed by this pop is disbanded
		void release_recruit();
	};
}

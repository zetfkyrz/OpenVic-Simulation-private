#include "Pop.hpp"

#include <algorithm>
#include <atomic>
#include <concepts> // IWYU pragma: keep for lambda
#include <cstddef>
#include <cstdint>
#include <optional>
#include <ranges>
#include <type_traits>

#include <fmt/std.h>

#include <type_safe/strong_typedef.hpp>

#include "openvic-simulation/misc/MoneyLedger.hpp"
#include "openvic-simulation/core/error/ErrorMacros.hpp"
#include "openvic-simulation/core/object/FixedPoint.hpp"
#include "openvic-simulation/core/object/FixedPoint/Math.hpp"
#include "openvic-simulation/core/stl/containers/TypedSpan.hpp"
#include "openvic-simulation/core/Typedefs.hpp"
#include "openvic-simulation/country/CountryDefinition.hpp"
#include "openvic-simulation/country/CountryInstance.hpp"
#include "openvic-simulation/country/CountryParty.hpp"
#include "openvic-simulation/defines/MilitaryDefines.hpp"
#include "openvic-simulation/defines/OpenVicDefines.hpp"
#include "openvic-simulation/defines/PopsDefines.hpp"
#include "openvic-simulation/economy/GoodDefinition.hpp"
#include "openvic-simulation/economy/GoodInstance.hpp"
#include "openvic-simulation/economy/production/ArtisanalProducer.hpp"
#include "openvic-simulation/economy/production/ProductionType.hpp"
#include "openvic-simulation/economy/trading/BuyResult.hpp"
#include "openvic-simulation/economy/trading/BuyUpToOrder.hpp"
#include "openvic-simulation/economy/trading/MarketInstance.hpp"
#include "openvic-simulation/economy/trading/MarketSellOrder.hpp"
#include "openvic-simulation/economy/trading/SellResult.hpp"
#include "openvic-simulation/map/ProvinceInstance.hpp"
#include "openvic-simulation/map/State.hpp"
#include "openvic-simulation/modifier/ModifierEffectCache.hpp"
#include "openvic-simulation/politics/Reform.hpp"
#include "openvic-simulation/population/Culture.hpp"
#include "openvic-simulation/population/PopNeedsMacro.hpp"
#include "openvic-simulation/population/PopType.hpp"
#include "openvic-simulation/population/PopValuesFromProvince.hpp"
#include "openvic-simulation/population/PopsAggregateDeps.hpp"
#include "openvic-simulation/population/Religion.hpp"
#include "openvic-simulation/types/ConstructorTags.hpp"
#include "openvic-simulation/types/fixed_point/FixedPointMap.hpp"
#include "openvic-simulation/types/OrderedContainers.hpp"
#include "openvic-simulation/types/TypedIndices.hpp"
#include "openvic-simulation/utility/Logger.hpp"

#include "PopDeps.hpp"

using namespace OpenVic;

constexpr pop_size_t TRUNCATION_ACCEPTABLE_BELOW_SIZE = 16;

static_assert(std::is_trivially_copyable_v<fixed_point_t>);

// The market callbacks of different goods run at the same time and add to the same pop (an artisan that sells two goods, the owners
// of several RGOs of a state). The sums that are not atomic members are added with a compare and swap, so no addition is lost
// (a lost addition of the artisanal revenue changed what the artisan decided on the next day).
static void add_atomically(fixed_point_t& sum, const fixed_point_t amount) {
	std::atomic_ref<fixed_point_t> sum_ref { sum };
	fixed_point_t expected = sum_ref.load(std::memory_order_relaxed);
	while (!sum_ref.compare_exchange_weak(expected, expected + amount, std::memory_order_relaxed)) {}
}

PopBase::PopBase(
	PopType const& new_type, Culture const& new_culture, Religion const& new_religion, pop_size_t new_size,
	fixed_point_t new_militancy, fixed_point_t new_consciousness, RebelType const* new_rebel_type
) : type { new_type }, culture { new_culture }, religion { new_religion }, size { new_size }, militancy { new_militancy },
	consciousness { new_consciousness }, rebel_type { new_rebel_type } {}

Pop::Pop(
	ProvinceInstance& new_location,
	PopBase const& pop_base,
	PopDeps const& pop_deps,
	const pop_id_in_province_t new_id_in_province
) : PopBase { pop_base },
	location { new_location },
  	id_in_province { new_id_in_province },
	market_instance { pop_deps.market_instance },
	artisanal_producer_optional {
		pop_base.get_type().is_artisan
			? std::optional<ArtisanalProducer> {
				pop_deps.artisanal_producer_deps
			}
			: std::optional<ArtisanalProducer> {}
	},
	supporter_equivalents_by_ideology { generate_values, pop_deps.pops_aggregate_deps.ideology_count },
	supporter_equivalents_by_party_policy { generate_values, pop_deps.pops_aggregate_deps.party_policy_count },
	supporter_equivalents_by_reform { generate_values, pop_deps.pops_aggregate_deps.reform_count } {
		reserve_needs_fulfilled_goods();
	}

fixed_point_t Pop::get_unemployment_fraction() const {
	if (!get_type().can_be_unemployed || size <= pop_size_t(0)) {
		return 0;
	}
	return fp::from_fraction(get_unemployed(), size);
}

void Pop::setup_pop_test_values(TypedSpan<reform_index_t, const Reform> reforms) {
	get_location().mark_political_support_dirty();

	/* Returns +/- range% of size. */
	const auto test_size = [this](int32_t range) -> pop_size_t {
		return size * ((rand() % (2 * range + 1)) - range) / 100;
	};

	num_grown = test_size(5);
	num_promoted = test_size(1);
	num_demoted = test_size(1);
	num_migrated_internal = test_size(3);
	num_migrated_external = test_size(1);
	num_migrated_colonial = test_size(2);

	total_change =
		num_grown + num_promoted + num_demoted + num_migrated_internal + num_migrated_external + num_migrated_colonial;

	/* Generates a number between 0 and max (inclusive) and sets map[&key] to it if it's at least min. */
	static auto fill_span_with_test_weights = [](std::span<fixed_point_t> span, int32_t min, int32_t max) -> void {
		for (fixed_point_t& item : span) {
			const int32_t value = rand() % (max + 1);
			item = value >= min ? value : 0;
		}
	};
	static auto rescale_span = [](std::span<fixed_point_t> span, const fixed_point_t new_total) -> void {
		fixed_point_t old_total = 0;
		for (const fixed_point_t item : span) {
			old_total += item;
		}

		if (old_total == 0) { return; }

		for (fixed_point_t& item : span) {
			item = fp::mul_div(item, new_total, old_total);
		}
	};
	static auto rescale_spans = [](
		std::span<fixed_point_t> span_a,
		std::span<fixed_point_t> span_b,
		const fixed_point_t new_total
	) -> void {
		fixed_point_t old_total = 0;
		for (const fixed_point_t item : span_a) {
			old_total += item;
		}
		for (const fixed_point_t item : span_b) {
			old_total += item;
		}

		if (old_total == 0) { return; }

		for (fixed_point_t& item : span_a) {
			item = fp::mul_div(item, new_total, old_total);
		}
		for (fixed_point_t& item : span_b) {
			item = fp::mul_div(item, new_total, old_total);
		}
	};
	static auto test_weight_ordered = []<typename T, typename U>(ordered_map<T const*, fixed_point_t>& map, U const& key, int32_t min, int32_t max) -> void {
		if constexpr (std::is_convertible_v<U const*, T const*> || std::is_convertible_v<U, T const*>) {
			const int32_t value = rand() % (max + 1);
			if (value >= min) {
				if constexpr (std::is_pointer_v<U>) {
					map.insert_or_assign(key, value);
				} else {
					map.insert_or_assign(&key, value);
				}
			}
		} else {
			// Optional: Handle the case where the conversion is not possible, perhaps with a static_assert.
			static_assert(std::is_convertible_v<U const*, T const*> || std::is_convertible_v<U, T const*>, "Type U is not convertible to T const*.");
		}
	};

	/* All entries equally weighted for testing. */
	fill_span_with_test_weights(supporter_equivalents_by_ideology, 1, 5);
	rescale_span(supporter_equivalents_by_ideology, type_safe::get(size));

	fill_span_with_test_weights(supporter_equivalents_by_party_policy, 3, 6);	
	fill_span_with_test_weights(supporter_equivalents_by_reform, 3, 6);
	for (Reform const& reform : reforms) {
		if (reform.group.is_civilizing()) {
			supporter_equivalents_by_reform[reform.index] = 0;
		}
	}
	rescale_spans(
		supporter_equivalents_by_party_policy,
		supporter_equivalents_by_reform,
		type_safe::get(size)
	);

	if (!vote_equivalents_by_party.empty()) {
		for (auto& [party, value] : vote_equivalents_by_party) {
			vote_equivalents_by_party[party] = 0;
			test_weight_ordered(vote_equivalents_by_party, party, 4, 10);
		}
		rescale_fixed_point_map(vote_equivalents_by_party, type_safe::get(size));
	}

	/* Returns a fixed point between 0 and max. */
	const auto test_range = [](fixed_point_t max = 1) -> fixed_point_t {
		return (rand() % 256) * max / 256;
	};

	cash = test_range(20);
	income = test_range(5);
	expenses = test_range(5);
	savings = test_range(15);
	life_needs_acquired_quantity = test_range();
	everyday_needs_acquired_quantity = test_range();
	luxury_needs_acquired_quantity = test_range();
	life_needs_desired_quantity = everyday_needs_desired_quantity = luxury_needs_desired_quantity = 1;
}

bool Pop::convert_to_equivalent() {
	PopType const& pop_type = type.get();
	PopType const* const equivalent = pop_type.get_equivalent();
	if (equivalent == nullptr) {
		spdlog::error_s("Tried to convert pop of type {} to equivalent, but there is no equivalent.", pop_type);
		return false;
	}

	type = *equivalent;
	reserve_needs_fulfilled_goods();
	return true;
}

void Pop::update_location_based_attributes() {
	get_location().mark_political_support_dirty();
	vote_equivalents_by_party.clear();
	CountryInstance const* owner = get_location().get_owner();
	if (owner == nullptr) {
		return;
	}
	CountryDefinition const& country_definition = owner->country_definition;

	auto view = country_definition.get_parties() | std::views::transform(
		[](CountryParty const& key) {
			return std::make_pair(&key, fixed_point_t::_0);
		}
	);
	vote_equivalents_by_party.insert(view.begin(), view.end());
	// TODO - calculate vote distribution
}

fixed_point_t Pop::get_vote_equivalents_by_party(CountryParty const& party) const {
	const decltype(vote_equivalents_by_party)::const_iterator it = vote_equivalents_by_party.find(&party);
	if (it == vote_equivalents_by_party.end()) {
		return 0;
	}
	return it.value();
}

void Pop::update_gamestate(
	MilitaryDefines const& military_defines,
	CountryInstance const* owner,
	const fixed_point_t pop_size_per_regiment_multiplier
) {
	using enum culture_status_t;

	if (owner != nullptr) {
		if (owner->is_primary_culture(culture)) {
			culture_status = PRIMARY;
		} else if (owner->is_accepted_culture(culture)) {
			culture_status = ACCEPTED;
		} else {
			culture_status = UNACCEPTED;
		}
	} else {
		culture_status = UNACCEPTED;
	}

	static constexpr fixed_point_t MIN_MILITANCY = 0;
	static constexpr fixed_point_t MAX_MILITANCY = 10;
	static constexpr fixed_point_t MIN_CONSCIOUSNESS = 0;
	static constexpr fixed_point_t MAX_CONSCIOUSNESS = 10;
	static constexpr fixed_point_t MIN_LITERACY = fixed_point_t::_0_01;
	static constexpr fixed_point_t MAX_LITERACY = 1;

	militancy = std::clamp(militancy, MIN_MILITANCY, MAX_MILITANCY);
	consciousness = std::clamp(consciousness, MIN_CONSCIOUSNESS, MAX_CONSCIOUSNESS);
	literacy = std::clamp(literacy, MIN_LITERACY, MAX_LITERACY);

	if (
		size < military_defines.get_min_pop_size_for_regiment() || owner == nullptr ||
		!is_culture_status_allowed(owner->get_allowed_regiment_cultures(), culture_status)
	) {
		max_supported_regiments = 0;
	} else {
		max_supported_regiments = (
			type_safe::get(size) / (type_safe::get(military_defines.get_pop_size_per_regiment()) * pop_size_per_regiment_multiplier) //
		).floor<size_t>() + 1;
	}
}

memory::string Pop::get_pop_context_text() const {
	return memory::fmt::format(
		"location: {} type: {} culture: {} religion: {} size: {}",
		location, type, culture, religion, size
	);
}

void Pop::reserve_needs_fulfilled_goods() {
	PopType const& pop_type = type.get();
	#define RESERVE_NEEDS(need_category) \
		need_category##_needs.reserve(pop_type.get_##need_category##_needs().size()); \
		need_category##_needs_fulfilled_goods_list.reserve(pop_type.get_##need_category##_needs().size());

	OV_DO_FOR_ALL_NEED_CATEGORIES(RESERVE_NEEDS)
	#undef RESERVE_NEEDS
}

#define DEFINE_NEEDS_FULFILLED_GOODS(need_category) \
	ordered_map<good_index_t, bool> Pop::get_##need_category##_needs_fulfilled_goods() const { \
		ordered_map<good_index_t, bool> result; \
		if (!needs_fulfilled_goods_recorded) { \
			return result; \
		} \
		result.reserve(type.get().get_##need_category##_needs().size()); \
		for (auto [good, base_demand] : type.get().get_##need_category##_needs()) { \
			result.emplace( \
				good, \
				std::find( \
					need_category##_needs_fulfilled_goods_list.begin(), need_category##_needs_fulfilled_goods_list.end(), good \
				) != need_category##_needs_fulfilled_goods_list.end() \
			); \
		} \
		return result; \
	}
OV_DO_FOR_ALL_NEED_CATEGORIES(DEFINE_NEEDS_FULFILLED_GOODS)
#undef DEFINE_NEEDS_FULFILLED_GOODS

void Pop::pay_income_tax(fixed_point_t& income) {
	CountryInstance* const tax_collector_nullable = get_location().get_country_to_report_economy();
	if (tax_collector_nullable == nullptr) {
		return;
	}
	fixed_point_t effective_tax_rate = tax_collector_nullable->get_effective_tax_rate_by_strata(get_type().strata).get_untracked();
	// The taxation of the laws: who pays more and who less (graduated, per capita, land...)
	{
		std::string_view const strata = get_type().strata.get_identifier();
		const fixed_point_t law = tax_collector_nullable->get_law_effect(
			strata == "rich" ? law_effect_t::tax_rich : strata == "middle" ? law_effect_t::tax_middle : law_effect_t::tax_poor
		) + (get_type().get_identifier() == "clergymen" ? tax_collector_nullable->get_law_effect(law_effect_t::tax_clergy) : fixed_point_t::_0);
		if (law != fixed_point_t::_0) {
			effective_tax_rate = std::clamp(effective_tax_rate * (fixed_point_t::_1 + law), fixed_point_t::_0, fixed_point_t::_1);
		}
	}
	// The bureaucrats of the state collect the taxes (version 88, docs/ADMINISTRATION.md): the rest stays with the pop
	if (State const* const state = get_location().get_state(); state != nullptr) {
		effective_tax_rate = effective_tax_rate * state->get_admin_collection();
	}
	const fixed_point_t tax = effective_tax_rate * income;
	MoneyLedger::record(MoneyLedger::label_t::pop_tax_paid, -tax);
	tax_collector_nullable->report_pop_income_tax(type, income, tax);
	income -= tax;
}

template<bool IsTaxable>
void Pop::add_artisanal_revenue(const fixed_point_t revenue) {
	if (OV_unlikely(revenue == 0)) {
		if (size >= TRUNCATION_ACCEPTABLE_BELOW_SIZE) {
			spdlog::warn_s("Adding artisanal_revenue of 0 to pop. Context{}", get_pop_context_text());
		}
		return;
	}

	if (OV_unlikely(revenue < 0)) {
		spdlog::error_s("Adding negative artisanal_revenue of {} to pop. Context{}", revenue, get_pop_context_text());
		return;
	}

	fixed_point_t income;
	MoneyLedger::record(MoneyLedger::label_t::gross_income_other, revenue);
	if constexpr (IsTaxable) {
		// Only the profit is taxed, but the pop gets the whole revenue: it paid for the inputs out of its own cash
		// (add_artisan_inputs_expense). Crediting only the profit destroyed the cost of the inputs a second time.
		fixed_point_t taxable = revenue;
		if (OV_likely(artisanal_producer_optional.has_value())) {
			taxable = std::max(
				fixed_point_t::_0,
				revenue - artisanal_producer_optional->get_costs_of_production()
			);
		}
		const fixed_point_t taxable_before_tax = taxable;
		pay_income_tax(taxable);
		income = revenue - (taxable_before_tax - taxable);
	} else {
		income = revenue;
	}

	add_atomically(artisanal_revenue, revenue);
	// The local income shadows the member: `income += income` doubled the income of the artisans (money out of nothing)
	add_atomically(this->income, income);
	cash += income;
	MoneyLedger::record(MoneyLedger::label_t::pop_artisanal_revenue, income);
}
template void Pop::add_artisanal_revenue<true>(const fixed_point_t revenue);
template void Pop::add_artisanal_revenue<false>(const fixed_point_t revenue);

#define DEFINE_ADD_INCOME_FUNCTIONS(name) \
	void Pop::add_##name(fixed_point_t amount){ \
		if (OV_unlikely(amount == 0)) { \
			if (size >= TRUNCATION_ACCEPTABLE_BELOW_SIZE) { \
				spdlog::warn_s("Adding " #name " of 0 to pop. Context{}", get_pop_context_text()); \
			} \
			return; \
		} \
		if (OV_unlikely(amount < 0)) { \
			spdlog::error_s("Adding negative " #name " of {} to pop. Context{}", amount, get_pop_context_text()); \
			return; \
		} \
		MoneyLedger::record_gross_income(#name, amount); \
		pay_income_tax(amount); \
		add_atomically(name, amount); \
		add_atomically(income, amount); \
		cash += amount; \
		MoneyLedger::record(MoneyLedger::label_t::pop_##name, amount); \
	}

OV_DO_FOR_ALL_TYPES_OF_POP_INCOME(DEFINE_ADD_INCOME_FUNCTIONS)
#undef DEFINE_ADD_INCOME_FUNCTIONS

#define DEFINE_ADD_EXPENSE_FUNCTIONS(name) \
	void Pop::add_##name(const fixed_point_t amount){ \
		if (OV_unlikely(amount == 0)) { \
			if (size >= TRUNCATION_ACCEPTABLE_BELOW_SIZE) { \
				spdlog::warn_s("Adding " #name " of 0 to pop. Context:{}", get_pop_context_text()); \
			} \
			return; \
		} \
		add_atomically(name, amount); \
		const fixed_point_t expenses_copy = expenses += amount; \
		if (OV_unlikely(expenses_copy < 0)) { \
			spdlog::error_s( \
				"Total expenses became negative ({}) after adding " #name " of {} to pop. Context:{}", \
				expenses_copy, amount, get_pop_context_text() \
			); \
		} \
		const fixed_point_t cash_copy = cash -= amount; \
		MoneyLedger::record(MoneyLedger::label_t::pop_##name, -amount); \
		if (OV_unlikely(cash_copy < 0)) { \
			spdlog::error_s( \
				"Total cash became negative ({}) after adding " #name " of {} to pop. Context:{}", \
				cash_copy, amount, get_pop_context_text() \
			); \
		} \
	}

OV_DO_FOR_ALL_TYPES_OF_POP_EXPENSES(DEFINE_ADD_EXPENSE_FUNCTIONS)
#undef DEFINE_ADD_EXPENSE_FUNCTIONS

void Pop::add_import_subsidies(const fixed_point_t amount) {
	//It's not income, otherwise we'd pay income tax.
	//It's not a negative expense, otherwise total expense might go negative.
	cash += amount;
	MoneyLedger::record(MoneyLedger::label_t::pop_import_subsidies, amount);
}

#define DEFINE_NEEDS_FULFILLED(need_category) \
	fixed_point_t Pop::get_##need_category##_needs_fulfilled() const { \
		const fixed_point_t desired_quantity_copy = need_category##_needs_desired_quantity.get_copy_of_value(); \
		if (desired_quantity_copy == 0) { \
			return 1; \
		} \
		return need_category##_needs_acquired_quantity.get_copy_of_value() / desired_quantity_copy; \
	}
OV_DO_FOR_ALL_NEED_CATEGORIES(DEFINE_NEEDS_FULFILLED)
#undef DEFINE_NEEDS_FULFILLED

void Pop::allocate_for_needs(
	need_list_t const& scaled_needs,
	forwardable_span<fixed_point_t> money_to_spend_per_good,
	memory::vector<fixed_point_t>& reusable_vector,
	fixed_point_t& weights_sum,
	fixed_point_t& cash_left_to_spend
) {
	if (weights_sum <= 0) {
		return;
	}

	// The prices do not change while the pops are ticked, so what depends only on the good is worked out once and not in every round below.
	// reusable_vector: [0, count) the money planned for every good, [count, 2 count) the most money the good can use,
	// [2 count, 3 count) its weight (the inverse of its price)
	const std::size_t count = scaled_needs.size();
	reusable_vector.clear();
	reusable_vector.resize(3 * count);
	fixed_point_t* const money_to_spend_per_good_draft = reusable_vector.data();
	fixed_point_t* const max_money_to_spend_per_good = money_to_spend_per_good_draft + count;
	fixed_point_t* const weight_per_good = max_money_to_spend_per_good + count;
	for (std::size_t i = 0; i < count; ++i) {
		auto const& [good_index, max_quantity_to_buy] = scaled_needs[i];
		max_money_to_spend_per_good[i] = market_instance.get_max_money_to_allocate_to_buy_quantity(good_index, max_quantity_to_buy);
		weight_per_good[i] = market_instance.get_good_instance(good_index).get_price_inverse();
	}
	fixed_point_t cash_left_to_spend_draft = cash_left_to_spend;

	bool needs_redistribution = true;
	while (needs_redistribution) {
		needs_redistribution = false;
		for (std::size_t i = 0; i < count; ++i) {
			const fixed_point_t max_money_to_spend = max_money_to_spend_per_good[i];
			if (money_to_spend_per_good_draft[i] >= max_money_to_spend) {
				continue;
			}

			const fixed_point_t weight = weight_per_good[i];
			const fixed_point_t cash_available_for_good = fp::mul_div(
				cash_left_to_spend_draft,
				weight,
				weights_sum
			);

			if (cash_available_for_good >= max_money_to_spend) {
				cash_left_to_spend_draft -= max_money_to_spend;
				money_to_spend_per_good_draft[i] = max_money_to_spend;
				weights_sum -= weight;
				needs_redistribution = weights_sum > 0;
				break;
			}

			const fixed_point_t max_possible_quantity_bought =
				cash_available_for_good / market_instance.get_min_next_price(scaled_needs[i].first);
			if (max_possible_quantity_bought < fixed_point_t::epsilon) {
				money_to_spend_per_good_draft[i] = 0;
			} else {
				money_to_spend_per_good_draft[i] = cash_available_for_good;
			}
		}
	}

	for (std::size_t i = 0; i < count; ++i) {
		const fixed_point_t money_to_spend = money_to_spend_per_good_draft[i];
		money_to_spend_per_good[type_safe::get(scaled_needs[i].first)] += money_to_spend;
		cash_left_to_spend -= money_to_spend;
	}

	reusable_vector.clear();
}

void Pop::pop_tick(
	PopValuesFromProvince const& shared_values,
	RandomU32& random_number_generator,
	TypedSpan<good_index_t, char> reusable_goods_mask,
	forwardable_span<
		memory::vector<fixed_point_t>,
		VECTORS_FOR_POP_TICK
	> reusable_vectors
) {
	pop_tick_without_cleanup(
		shared_values,
		random_number_generator,
		reusable_goods_mask,
		reusable_vectors
	);
	for (auto& reusable_vector : reusable_vectors) {
		reusable_vector.clear();
	}
}

void Pop::pop_tick_without_cleanup(
	PopValuesFromProvince const& shared_values,
	RandomU32& random_number_generator,
	TypedSpan<good_index_t, char> reusable_goods_mask,
	forwardable_span<
		memory::vector<fixed_point_t>,
		VECTORS_FOR_POP_TICK
	> reusable_vectors
) {
	const std::size_t good_count = type_safe::get(reusable_goods_mask.size());
	memory::vector<fixed_point_t>& reusable_vector_0 = reusable_vectors[0];
	memory::vector<fixed_point_t>& reusable_vector_1 = reusable_vectors[1];
	// The vectors are empty when a pop starts (see pop_tick). resize() without a value fills the new elements with zeros in a loop that
	// becomes a memset, resize(n, 0) copied the value into every element in a function of the library (a fifth of the instructions of
	// a pop tick)
	memory::vector<fixed_point_t>& max_quantity_to_buy_per_good = reusable_vectors[2];
	max_quantity_to_buy_per_good.resize(good_count);
	memory::vector<fixed_point_t>& money_to_spend_per_good = reusable_vectors[3];
	money_to_spend_per_good.resize(good_count);
	cash_allocated_for_artisanal_spending = 0;
	// A day of hunger (the needs of yesterday are still there, the new ones are filled below)
	if (population_defines != nullptr && size > pop_size_t(0)
		&& get_life_needs_fulfilled() < population_defines->hunger_life_needs_threshold.fixed()) {
		hungry_days++;
	}
	// Yesterday's fulfilled goods are forgotten, today's are recorded as the goods are bought (after_buy)
	#define FORGET_FULFILLED_GOODS(need_category) need_category##_needs_fulfilled_goods_list.clear();
	OV_DO_FOR_ALL_NEED_CATEGORIES(FORGET_FULFILLED_GOODS)
	#undef FORGET_FULFILLED_GOODS
	needs_fulfilled_goods_recorded = true;
	
	fixed_point_map_t<good_index_t> goods_to_sell {};
	// What the life needs of an artisan cost at the highest prices of today: kept back from the money of the inputs (the goods it keeps from its
	// work and from the RGO are not counted, a little more is kept than it will spend)
	cash_reserved_for_life_needs = fixed_point_t::_0;
	if (artisanal_producer_optional.has_value() && population_defines != nullptr && population_defines->artisan_life_needs_first != 0) {
		PopType const& own_type = type;
		const fixed_point_t life_scalar = (fixed_point_t::_1 + 2 * consciousness / shared_values.defines.get_pdef_base_con()) * size
			* population_defines->pop_needs_multiplier.fixed() * shared_values.get_effects_by_strata()[own_type.strata.index].get_shared_life_needs_scalar();
		if (life_scalar > fixed_point_t::_0) {
			for (auto [good_index, quantity] : own_type.get_life_needs()) {
				if (market_instance.get_is_available(good_index)) {
					cash_reserved_for_life_needs += market_instance.get_max_money_to_allocate_to_buy_quantity(
						good_index, quantity * life_scalar / size_denominator
					);
				}
			}
		}
	}
	if (artisanal_producer_optional.has_value()) {
		//execute artisan_tick before needs
		ArtisanalProducer& artisanal_producer = artisanal_producer_optional.value();
		artisanal_producer.artisan_tick(
			market_instance,
			*this,
			shared_values,
			random_number_generator,
			reusable_goods_mask,
			max_quantity_to_buy_per_good,
			money_to_spend_per_good,
			reusable_vector_0,
			reusable_vector_1,
			goods_to_sell
		);
	}

	// The consumption tax of the laws: before the expenses of yesterday are forgotten, the pop pays the share of the law of the tax rate of its
	// strata of what it spent on its needs yesterday (as much as it has)
	if (CountryInstance* const consumption_tax_collector = get_location().get_country_to_report_economy(); consumption_tax_collector != nullptr) {
		const fixed_point_t share = consumption_tax_collector->get_law_effect(law_effect_t::consumption_tax);
		const fixed_point_t spent = life_needs_expense + everyday_needs_expense + luxury_needs_expense;
		if (share > fixed_point_t::_0 && spent > fixed_point_t::_0) {
			const fixed_point_t rate = std::clamp(
				consumption_tax_collector->get_effective_tax_rate_by_strata(get_type().strata).get_untracked() * share, fixed_point_t::_0, fixed_point_t::_1
			);
			const fixed_point_t tax = std::min(rate * spent, std::max(fixed_point_t::_0, cash.get_copy_of_value()));
			if (tax > fixed_point_t::_0) {
				cash -= tax;
				MoneyLedger::record(MoneyLedger::label_t::pop_tax_paid, -tax);
				consumption_tax_collector->report_pop_income_tax(get_type(), fixed_point_t::_0, tax);
			}
		}
	}

	//after artisan_tick as it uses income & expenses
	#define SET_TO_ZERO(name) \
		name = 0;

	OV_DO_FOR_ALL_TYPES_OF_POP_INCOME(SET_TO_ZERO)
	OV_DO_FOR_ALL_TYPES_OF_POP_EXPENSES(SET_TO_ZERO)
	#undef SET_TO_ZERO
	income = expenses = 0;
	// Yesterday's revenue was read by the artisan tick above; start counting today's. It used to be summed up over the whole game, so an
	// artisan that had ever sold something looked profitable for ever and never changed its production type.
	artisanal_revenue = 0;

	CountryInstance* const country_to_report_economy_nullable = get_location().get_country_to_report_economy();

	if (country_to_report_economy_nullable != nullptr) {
		country_to_report_economy_nullable->request_salaries_and_welfare_and_import_subsidies(*this);
	}

	//unemployment subsidies are based on yesterdays unemployment
	employed = 0;
	//import subsidies are based on yesterday
	yesterdays_import_value = 0;

	PopType const& pop_type = type;
	PopStrataValuesFromProvince const& shared_strata_values = shared_values.get_effects_by_strata()[pop_type.strata.index];
	PopsDefines const& defines = shared_values.defines;
	// POP_NEEDS_MULTIPLIER: what the pops need of the quantities of the data (version 78: a half)
	const fixed_point_t base_needs_scalar = (
		fixed_point_t::_1 + 2 * consciousness / defines.get_pdef_base_con()
	) * size * (population_defines != nullptr ? population_defines->pop_needs_multiplier.fixed() : fixed_point_t::_1);

	// The goods the pop kept from the output of the RGO it works at (subsistence): eaten before anything is bought
	ResourceGatheringOperation const& location_rgo = get_location().get_rgo();
	fixed_point_t subsistence_left = location_rgo.get_subsistence_entitlement(*this);
	const good_index_t subsistence_good_index = location_rgo.get_subsistence_good_index();

	memory::vector<fixed_point_t>& need_demand_per_good = shared_values.pop_need_demand_per_good;
	#define FILL_NEEDS(need_category) \
		need_category##_needs.clear(); \
		const fixed_point_t need_category##_needs_scalar = base_needs_scalar * shared_strata_values.get_shared_##need_category##_needs_scalar(); \
		fixed_point_t need_category##_needs_price_inverse_sum = 0; \
		if (OV_likely(need_category##_needs_scalar > 0)) { \
			need_category##_needs_acquired_quantity = need_category##_needs_desired_quantity = 0; \
			for (auto [good_index, quantity] : pop_type.get_##need_category##_needs()) { \
				if (!market_instance.get_is_available(good_index)) { \
					continue; \
				} \
				fixed_point_t max_quantity_to_buy = quantity * need_category##_needs_scalar / size_denominator; \
				if (max_quantity_to_buy == 0) { \
					continue; \
				} \
				if (country_to_report_economy_nullable != nullptr) { \
					/* Added up here and sent to the country once per province (a lock for every good of every pop was slow) */ \
					need_demand_per_good[type_safe::get(good_index)] += max_quantity_to_buy; \
				} \
				need_category##_needs_desired_quantity += max_quantity_to_buy; \
				auto goods_to_sell_iterator = goods_to_sell.empty() ? goods_to_sell.end() : goods_to_sell.find(good_index); \
				if (goods_to_sell_iterator != goods_to_sell.end() && goods_to_sell_iterator.value() > 0) { \
					const fixed_point_t own_produce_consumed = std::min(goods_to_sell_iterator.value(), max_quantity_to_buy); \
					goods_to_sell_iterator.value() -= own_produce_consumed; \
					max_quantity_to_buy -= own_produce_consumed; \
					need_category##_needs_acquired_quantity += own_produce_consumed; \
					if (country_to_report_economy_nullable != nullptr) { \
						country_to_report_economy_nullable->report_pop_need_consumption(pop_type, good_index, own_produce_consumed); \
					} \
				} \
				if (subsistence_left > 0 && good_index == subsistence_good_index) { \
					const fixed_point_t subsistence_eaten = std::min(subsistence_left, max_quantity_to_buy); \
					subsistence_left -= subsistence_eaten; \
					location_rgo.record_subsistence_eaten(subsistence_eaten); \
					max_quantity_to_buy -= subsistence_eaten; \
					need_category##_needs_acquired_quantity += subsistence_eaten; \
					if (country_to_report_economy_nullable != nullptr) { \
						country_to_report_economy_nullable->report_pop_need_consumption(pop_type, good_index, subsistence_eaten); \
					} \
				} \
				if (OV_likely(max_quantity_to_buy > 0)) { \
					need_category##_needs_price_inverse_sum += market_instance.get_good_instance(good_index).get_price_inverse(); \
					need_category##_needs.emplace_back(good_index, max_quantity_to_buy); \
					max_quantity_to_buy_per_good[type_safe::get(good_index)] += max_quantity_to_buy; \
				} \
			} \
		}

	OV_DO_FOR_ALL_NEED_CATEGORIES(FILL_NEEDS)
	#undef FILL_NEEDS

	//It's safe to use cash as this happens before cash is updated via spending
	fixed_point_t cash_left_to_spend = cash.get_copy_of_value() / shared_values.get_max_cost_multiplier()
		- cash_allocated_for_artisanal_spending;

	#define ALLOCATE_FOR_NEEDS(need_category) \
		if (cash_left_to_spend > 0) { \
			allocate_for_needs( \
				need_category##_needs, \
				money_to_spend_per_good, \
				reusable_vector_0, \
				need_category##_needs_price_inverse_sum, \
				cash_left_to_spend \
			); \
		}

	OV_DO_FOR_ALL_NEED_CATEGORIES(ALLOCATE_FOR_NEEDS)
	#undef ALLOCATE_FOR_NEEDS

	const std::optional<country_index_t> country_index_optional = country_to_report_economy_nullable == nullptr
		? std::nullopt
		: std::optional<country_index_t>{country_to_report_economy_nullable->index};

	for (std::size_t i = 0; i < good_count; ++i) {
		const fixed_point_t max_quantity_to_buy = max_quantity_to_buy_per_good[i];

		if (max_quantity_to_buy <= 0) {
			continue;
		}
		
		const fixed_point_t money_to_spend = money_to_spend_per_good[i];

		market_instance.place_buy_up_to_order({
			good_index_t(i),
			country_index_optional,
			max_quantity_to_buy,
			money_to_spend,
			this,
			after_buy
		});
	}

	for (const auto [good_index, quantity_to_sell] : goods_to_sell) {
		if (quantity_to_sell <= 0) {
			if (OV_unlikely(quantity_to_sell < 0)) {
				spdlog::error_s("Pop had negative quantity {} left to sell of good {}.", quantity_to_sell, good_index);
			}
			continue;
		}

		market_instance.place_market_sell_order(
			{
				good_index,
				country_index_optional,
				quantity_to_sell,
				this,
				after_sell,
				get_location().get_freight_rate()
			},
			reusable_vectors[4]
		);
	}
}

void Pop::after_buy(void* actor, BuyResult const& buy_result) {
	const fixed_point_t quantity_bought = buy_result.quantity_bought;

	if (quantity_bought == 0) {
		return;
	}

	Pop& pop = *static_cast<Pop*>(actor);
	CountryInstance* const country_to_report_economy_nullable = pop.get_location().get_country_to_report_economy();

	fixed_point_t money_spent = buy_result.money_spent_total;
	pop.yesterdays_import_value += buy_result.money_spent_on_imports;
	if (country_to_report_economy_nullable != nullptr) {
		const fixed_point_t tariff = country_to_report_economy_nullable->apply_tariff(buy_result.money_spent_on_imports);
		money_spent += tariff;
	}

	const good_index_t good_index = buy_result.good_index;
	fixed_point_t quantity_left_to_consume = quantity_bought;
	CountryInstance* get_country_to_report_economy_nullable = pop.get_location().get_country_to_report_economy();

	#define CONSUME_NEED(need_category) \
		if (quantity_left_to_consume > 0) { \
			const need_list_t::const_iterator need_category##it = std::find_if( \
				pop.need_category##_needs.begin(), pop.need_category##_needs.end(), \
				[good_index](std::pair<good_index_t, fixed_point_t> const& need) -> bool { return need.first == good_index; } \
			); \
			if (need_category##it != pop.need_category##_needs.end()) { \
				const fixed_point_t desired_quantity = need_category##it->second; \
				fixed_point_t consumed_quantity; \
				if (quantity_left_to_consume >= desired_quantity) { \
					consumed_quantity = desired_quantity; \
					pop.need_category##_needs_fulfilled_goods_list.push_back(good_index); \
				} else { \
					consumed_quantity = quantity_left_to_consume; \
				} \
				pop.need_category##_needs_acquired_quantity += consumed_quantity; \
				quantity_left_to_consume -= consumed_quantity; \
				if (get_country_to_report_economy_nullable != nullptr) { \
					get_country_to_report_economy_nullable->report_pop_need_consumption(pop.type, good_index, consumed_quantity); \
				} \
				const fixed_point_t expense = fp::mul_div( \
					money_spent, \
					consumed_quantity, \
					quantity_bought \
				); \
				if (expense > 0) { \
					pop.add_##need_category##_needs_expense(expense); \
				} \
			} \
		}

	// An artisan eats before it fills the stockpile of its work (ARTISAN_LIFE_NEEDS_FIRST): the goods of its life needs (grain, fruit, wool, cattle,
	// fish) are the inputs of the liquor, the wine and the cloth, and when the market gave less than the order the stockpile took all of them
	const bool life_needs_first = pop.artisanal_producer_optional.has_value() && population_defines != nullptr
		&& population_defines->artisan_life_needs_first != 0;
	if (life_needs_first) {
		CONSUME_NEED(life)
	}
	if (pop.artisanal_producer_optional.has_value()) {
		if (quantity_left_to_consume <= 0) {
			return;
		}
		const fixed_point_t quantity_added_to_stockpile = pop.artisanal_producer_optional.value().add_to_stockpile(
			good_index,
			quantity_left_to_consume
		);

		if (quantity_added_to_stockpile > 0) {
			quantity_left_to_consume -= quantity_added_to_stockpile;
			const fixed_point_t expense = fp::mul_div(
				money_spent,
				quantity_added_to_stockpile,
				quantity_bought
			);
			// a share of a tiny purchase can round to nothing (it warned hundreds of times a game)
			if (expense > fixed_point_t::_0) {
				pop.add_artisan_inputs_expense(expense);
			}
		}
	}

	if (!life_needs_first) {
		CONSUME_NEED(life)
	}
	CONSUME_NEED(everyday)
	CONSUME_NEED(luxury)
	#undef CONSUME_NEED
}

void Pop::after_sell(void* actor, SellResult const& sell_result, memory::vector<fixed_point_t>& reusable_vector) {
	Pop& pop = *static_cast<Pop*>(actor);
	MoneyLedger::record(MoneyLedger::label_t::sell_callbacks_artisans, sell_result.money_gained);
	if (sell_result.money_gained > 0) {
		OV_ERR_FAIL_COND_MSG(!pop.artisanal_producer_optional.has_value(), "Pop is selling artisanal goods but has no artisan.");
		ArtisanalProducer& artisan = pop.artisanal_producer_optional.value();
		if (artisan.get_last_produced_good() != nullptr && artisan.get_last_produced_good()->index == sell_result.good_index) {
			pop.add_artisanal_revenue<true>(sell_result.money_gained);
		} else {
			pop.add_artisanal_revenue<false>(sell_result.money_gained);
		}
		artisan.subtract_from_stockpile(sell_result.good_index, sell_result.quantity_sold);
	}
}

void Pop::allocate_cash_for_artisanal_spending(fixed_point_t money_to_spend) {
	cash_allocated_for_artisanal_spending += money_to_spend;
}

fixed_point_t Pop::take_cash(const fixed_point_t amount) {
	const fixed_point_t available = cash.get_copy_of_value();
	const fixed_point_t taken = amount < available ? amount : available;
	if (taken <= fixed_point_t::_0) {
		return fixed_point_t::_0;
	}
	cash -= taken;
	MoneyLedger::record(MoneyLedger::label_t::pop_cash_taken, -taken);
	return taken;
}

void Pop::hire(pop_size_t count) {
	if (OV_unlikely(count <= 0)) {
		spdlog::warn_s(
			"Tried employing non-positive number of pops. {} Context{}",
			count, get_pop_context_text()
		);
	}
	employed += count;
	if (OV_unlikely(employed > size)) {
		spdlog::error_s(
			"Employed count became greater than pop size. {} Context{}",
			employed, get_pop_context_text()
		);
	} else if (OV_unlikely(employed < 0)) {
		spdlog::error_s(
			"Employed count became negative. {} Context{}",
			employed, get_pop_context_text()
		);
	}
}

bool Pop::try_recruit() {
	if (regiment_count >= max_supported_regiments) {
		return false;
	}

	++regiment_count;
	return true;
}

bool Pop::try_recruit_understrength() {
	if (regiment_count > max_supported_regiments) {
		return false;
	}

	++regiment_count;
	return true;
}

void Pop::add_recruit() {
	++regiment_count;
}

void Pop::release_recruit() {
	if (regiment_count > 0) {
		--regiment_count;
	}
}

void Pop::clear_dynamics_record() {
	total_change = 0;
	num_grown = 0;
	num_promoted = 0;
	num_demoted = 0;
	num_migrated_internal = 0;
	num_migrated_external = 0;
	num_migrated_colonial = 0;
	num_starved = 0;
}

void Pop::change_size(const std::int32_t delta, const std::int32_t kind) {
	std::int32_t applied = delta;
	if (type_safe::get(size) + applied < 0) {
		applied = -type_safe::get(size);
	}
	size += pop_size_t(applied);
	total_change += pop_size_t(applied);
	switch (kind) {
	case 0: num_grown += pop_size_t(applied); break;
	case 1: num_promoted += pop_size_t(applied); break;
	case 2: num_demoted += pop_size_t(applied); break;
	case 3: num_migrated_internal += pop_size_t(applied); break;
	case 4: num_migrated_external += pop_size_t(applied); break;
	case 5: num_starved += pop_size_t(applied); break;
	default: break;
	}
}

void Pop::set_political_support(
	std::span<const fixed_point_t> ideology_support, std::span<const fixed_point_t> party_policy_support,
	std::span<const fixed_point_t> reform_support
) {
	get_location().mark_political_support_dirty();
	for (size_t i = 0; i < ideology_support.size() && i < static_cast<size_t>(type_safe::get(supporter_equivalents_by_ideology.size())); ++i) {
		supporter_equivalents_by_ideology[ideology_index_t(i)] = ideology_support[i];
	}
	for (size_t i = 0; i < party_policy_support.size() && i < static_cast<size_t>(type_safe::get(supporter_equivalents_by_party_policy.size())); ++i) {
		supporter_equivalents_by_party_policy[party_policy_index_t(i)] = party_policy_support[i];
	}
	for (size_t i = 0; i < reform_support.size() && i < static_cast<size_t>(type_safe::get(supporter_equivalents_by_reform.size())); ++i) {
		supporter_equivalents_by_reform[reform_index_t(i)] = reform_support[i];
	}
}

void Pop::set_party_votes(fixed_point_map_t<CountryParty const*> const& votes) {
	get_location().mark_political_support_dirty();
	vote_equivalents_by_party.clear();
	for (auto const& entry : votes) {
		vote_equivalents_by_party[entry.first] = entry.second;
	}
}

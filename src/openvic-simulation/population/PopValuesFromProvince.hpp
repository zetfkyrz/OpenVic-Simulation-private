#pragma once

#include <cstddef>
#include <utility>

#include "openvic-simulation/core/memory/FixedVector.hpp"
#include "openvic-simulation/core/memory/Vector.hpp"
#include "openvic-simulation/core/object/FixedPoint.hpp"
#include "openvic-simulation/types/TypedIndices.hpp"
#include "openvic-simulation/utility/Getters.hpp"

namespace OpenVic {
	struct GameRulesManager;
	struct GoodInstanceManager;
	struct ModifierEffectCache;
	struct ProductionType;
	struct ProductionTypeManager;
	struct ProvinceInstance;
	struct PopsDefines;
	struct PopValuesFromProvince;
	struct Strata;

	struct PopStrataValuesFromProvince {
	private:
		const strata_index_t strata_index;
		fixed_point_t PROPERTY(shared_life_needs_scalar);
		fixed_point_t PROPERTY(shared_everyday_needs_scalar);
		fixed_point_t PROPERTY(shared_luxury_needs_scalar);
	public:
		constexpr PopStrataValuesFromProvince(const strata_index_t new_strata_index)
			: strata_index { new_strata_index } {}

		void update_pop_strata_values_from_province(
			PopsDefines const& defines,
			ModifierEffectCache const& modifier_effect_cache,
			ProvinceInstance const& province
		);
	};

	struct PopValuesFromProvince {
	private:
		GoodInstanceManager const& good_instance_manager;
		ModifierEffectCache const& modifier_effect_cache;
		ProductionTypeManager const& production_type_manager;
		fixed_point_t PROPERTY(max_cost_multiplier);
		memory::FixedVector<PopStrataValuesFromProvince, strata_index_t> PROPERTY(effects_by_strata);
		//excludes availability of goods on market
		memory::vector<std::pair<ProductionType const*, fixed_point_t>> SPAN_PROPERTY(ranked_artisanal_production_types);

		// The ranking above only depends on whether the province may use the coastal production types and on max_cost_multiplier (the
		// prices do not change while the provinces are ticked), so a ranking that was worked out is kept until clear_ranking_cache()
		struct ranking_cache_entry_t {
			fixed_point_t max_cost_multiplier;
			bool may_use_coastal_production_types;
			memory::vector<std::pair<ProductionType const*, fixed_point_t>> ranked_artisanal_production_types;
		};
		static constexpr std::size_t MAX_RANKING_CACHE_ENTRIES = 64;
		memory::vector<ranking_cache_entry_t> ranking_cache;
	public:
		PopsDefines const& defines;
		GameRulesManager const& game_rules_manager;

		PopValuesFromProvince(
			GameRulesManager const& new_game_rules_manager,
			GoodInstanceManager const& new_good_instance_manager,
			ModifierEffectCache const& new_modifier_effect_cache,
			ProductionTypeManager const& new_production_type_manager,
			PopsDefines const& new_defines,
			const strata_index_t strata_size
		);

		// Scratch of the thread that ticks the pops of a province: the demand of the pops for the goods of their needs, by good (the pops add
		// to it, the province sends it to its country once and clears it, see ProvinceInstance::province_tick)
		mutable memory::vector<fixed_point_t> pop_need_demand_per_good;

		void update_pop_values_from_province(ProvinceInstance& province);

		// To be called before the provinces are ticked: the prices of the goods may have changed since the rankings were worked out
		void clear_ranking_cache() {
			ranking_cache.clear();
		}
	};
}
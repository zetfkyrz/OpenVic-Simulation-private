#pragma once

#include <span>

#include <openvic-dataloader/v2script/Parser.hpp>

#include "openvic-simulation/core/memory/Vector.hpp"
#include "openvic-simulation/core/object/FixedPoint.hpp"
#include "openvic-simulation/core/stl/containers/TypedSpan.hpp"
#include "openvic-simulation/population/PopSize.hpp"
#include "openvic-simulation/economy/production/RgoBuilding.hpp"
#include "openvic-simulation/scripts/ConditionScript.hpp"
#include "openvic-simulation/types/IdentifierRegistry.hpp"
#include "openvic-simulation/types/IndexedFlatMap.hpp"
#include "openvic-simulation/types/PopSprite.hpp"
#include "openvic-simulation/types/TypedIndices.hpp"

namespace OpenVic {
	struct PopType;

	struct Job {
		enum struct effect_t { THROUGHPUT, INPUT, OUTPUT };

	public:
		const pop_type_index_t pop_type_index;
		const effect_t effect_type;
		const fixed_point_t effect_multiplier;
		const fixed_point_t amount;
		
		constexpr Job(
			const pop_type_index_t new_pop_type_index,
			const effect_t new_effect_type,
			const fixed_point_t new_effect_multiplier,
			const fixed_point_t new_amount
		) : pop_type_index { new_pop_type_index },
			effect_type { new_effect_type },
			effect_multiplier { new_effect_multiplier },
			amount { new_amount } {}

		constexpr Job() : Job({}, effect_t::THROUGHPUT, fixed_point_t::_0, fixed_point_t::_0) {};
	};

	struct GameRulesManager;
	struct GoodDefinition;
	struct GoodDefinitionManager;
	struct PopManager;
	struct ProvinceInstance;
	struct State;

	struct ProductionType : HasIdentifier {
		friend struct ProductionTypeManager;

		enum struct template_type_t { FACTORY, RGO, ARTISAN };

		using bonus_t = std::pair<ConditionScript, fixed_point_t>;

	private:
		GameRulesManager const& game_rules_manager;
		const memory::vector<Job> SPAN_PROPERTY(jobs);

		memory::vector<bonus_t> SPAN_PROPERTY(bonuses);

		const bool _is_farm, _is_mine, is_coastal;
		bool parse_scripts(DefinitionManager const& definition_manager);

	public:
		const std::optional<Job> owner;
		const fixed_point_map_t<GoodDefinition const*> input_goods;
		const fixed_point_map_t<GoodDefinition const*> maintenance_requirements;
		GoodDefinition const& output_good;
		const template_type_t template_type;
		const pop_size_t base_workforce_size;
		fixed_point_t base_output_quantity; // not const: the multipliers of the defines are applied once after the loading (ProductionTypeManager::scale_rgo_output)
		// An artisan type that makes a weapon: the unlock of the country its artisans need to make it (ARTISAN_WEAPON_FREE_TIER, ARTISAN_WEAPON_TIER_LAG),
		// set once after the loading (ProductionTypeManager::set_artisan_weapon_unlocks); empty: no unlock is needed
		memory::string artisan_unlock_key;

		ProductionType(
			GameRulesManager const& new_game_rules_manager,
			const std::string_view new_identifier,
			std::optional<Job>&& new_owner,
			memory::vector<Job>&& new_jobs,
			const template_type_t new_template_type,
			const pop_size_t new_base_workforce_size,
			fixed_point_map_t<GoodDefinition const*>&& new_input_goods,
			GoodDefinition const& new_output_good,
			const fixed_point_t new_base_output_quantity,
			memory::vector<bonus_t>&& new_bonuses,
			fixed_point_map_t<GoodDefinition const*>&& new_maintenance_requirements,
			const bool new_is_coastal,
			const bool new_is_farm,
			const bool new_is_mine
		);

		constexpr bool get_is_farm_for_non_tech() const {
			return _is_farm;
		}
		constexpr bool get_is_mine_for_tech() const {
			return _is_mine;
		}
		bool get_is_farm_for_tech() const;
		bool get_is_mine_for_non_tech() const;
		bool is_valid_for_artisan_in(ProvinceInstance& province) const;
		bool is_valid_for_factory_in(State const& state) const;
		ProductionType(ProductionType&&) = default;
	};

	// A production type that a factory of a building can run instead of the one of the building (a generation of a weapon): the files of
	// ov_data/common/ov_production_types, the key "building" names the building type
	struct FactoryRecipe {
		ProductionType const* production_type;
		memory::string building;
	};

	struct EquipmentManager;

	struct ProductionTypeManager {
	private:
		memory::vector<FactoryRecipe> extra_recipes;
		RgoBuildingData rgo_buildings;
		IdentifierRegistry<ProductionType> IDENTIFIER_REGISTRY(production_type);
		pop_sprite_t PROPERTY(rgo_owner_sprite, 0);
		OV_IFLATMAP_PROPERTY(GoodDefinition, ProductionType const*, good_to_rgo_production_type);

		NodeTools::node_callback_t _expect_job(
			GoodDefinitionManager const& good_definition_manager, PopManager const& pop_manager,
			NodeTools::callback_t<
				pop_type_index_t,
				Job::effect_t,
				fixed_point_t,
				fixed_point_t
			> emplace_callback
		);
		NodeTools::node_callback_t _expect_job_list(
			GoodDefinitionManager const& good_definition_manager, PopManager const& pop_manager,
			NodeTools::callback_t<memory::vector<Job>&&> callback
		);

	public:
		constexpr ProductionTypeManager()
			: good_to_rgo_production_type { decltype(good_to_rgo_production_type){create_empty} } {}

		bool add_production_type(
			GameRulesManager const& game_rules_manager,
			TypedSpan<pop_type_index_t, const PopType> pop_types,
			const std::string_view identifier,
			std::optional<Job>&& owner,
			memory::vector<Job>&& jobs,
			const ProductionType::template_type_t template_type,
			const pop_size_t base_workforce_size,
			fixed_point_map_t<GoodDefinition const*>&& input_goods,
			GoodDefinition const* const output_good,
			const fixed_point_t base_output_quantity,
			memory::vector<ProductionType::bonus_t>&& bonuses,
			fixed_point_map_t<GoodDefinition const*>&& maintenance_requirements,
			const bool is_coastal,
			const bool is_farm,
			const bool is_mine
		);

		bool load_production_types_file(
			GameRulesManager const& game_rules_manager,
			GoodDefinitionManager const& good_definition_manager,
			PopManager const& pop_manager,
			ovdl::v2script::Parser const& parser,
			std::span<ovdl::v2script::Parser const* const> extra_parsers = {}
		);
		// The production types of the files of ov_production_types: what a building can make besides its own type
		[[nodiscard]] RgoBuildingData& get_rgo_buildings() {
			return rgo_buildings;
		}
		[[nodiscard]] RgoBuildingData const& get_rgo_buildings() const {
			return rgo_buildings;
		}
		[[nodiscard]] std::span<const FactoryRecipe> get_extra_recipes() const {
			return extra_recipes;
		}

		// Multiplies the output of the resource gathering operation of the good (the base of the files is kept elsewhere as it is: once, after the loading)
		// The unlocks of the artisan types of the weapons (see ProductionType::artisan_unlock_key)
		void set_artisan_weapon_unlocks(EquipmentManager const& equipment_manager, const std::int32_t free_tier, const std::int32_t tier_lag);
		void scale_rgo_output(GoodDefinition const& good, fixed_point_t multiplier);

		bool parse_scripts(DefinitionManager const& definition_manager);
	};
}
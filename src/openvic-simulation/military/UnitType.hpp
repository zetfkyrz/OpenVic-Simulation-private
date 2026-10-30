#pragma once

#include <cstdint>
#include <string_view>
#include <vector>

#include <openvic-dataloader/v2script/Parser.hpp>

#include "openvic-simulation/core/object/FixedPoint.hpp"
#include "openvic-simulation/core/object/Timespan.hpp"
#include "openvic-simulation/dataloader/NodeTools.hpp"
#include "openvic-simulation/economy/GoodDefinition.hpp"
#include "openvic-simulation/modifier/Modifier.hpp"
#include "openvic-simulation/types/HasIdentifier.hpp"
#include "openvic-simulation/types/HasIndex.hpp"
#include "openvic-simulation/types/IdentifierRegistry.hpp"
#include "openvic-simulation/types/TypedIndices.hpp"
#include "openvic-simulation/types/UnitBranchType.hpp"

namespace OpenVic {
	struct TerrainType;
	struct TerrainTypeManager;
	struct Culture;
	struct CountryInstance;
	struct EquipmentManager;

	struct UnitType : HasIdentifier {
		using icon_t = uint32_t;
		using terrain_modifiers_t = ordered_map<TerrainType const*, Modifier>;

		enum struct unit_category_t : uint8_t {
			INVALID_UNIT_CATEGORY, INFANTRY, CAVALRY, SUPPORT, SPECIAL, BIG_SHIP, LIGHT_SHIP, TRANSPORT
		};

		struct unit_type_args_t {
			using terrain_modifier_values_t = ordered_map<TerrainType const*, ModifierValue>;

			icon_t icon = 0;
			unit_category_t unit_category = unit_category_t::INVALID_UNIT_CATEGORY;
			// TODO defaults for move_sound and select_sound
			std::string_view sprite, move_sound, select_sound;
			bool starts_unlocked = true, floating_flag = false;
			uint32_t priority = 0;
			fixed_point_t max_strength = 0, default_organisation = 0, maximum_speed = 0, weighted_value = 0,
				supply_consumption = 0;
			Timespan build_time;
			fixed_point_map_t<GoodDefinition const*> build_cost, supply_cost;
			terrain_modifier_values_t terrain_modifier_values;

			unit_type_args_t() {};
			unit_type_args_t(unit_type_args_t&&) = default;
		};

	private:
		memory::string PROPERTY(sprite);
		memory::string PROPERTY(move_sound);
		memory::string PROPERTY(select_sound);

		fixed_point_map_t<GoodDefinition const*> PROPERTY(build_cost);
		fixed_point_map_t<GoodDefinition const*> PROPERTY(supply_cost);

	public:
		// What the unit uses of a supply good each month: the part of its construction cost (the share is set by the defines of the
		// army and the navy: durable goods and consumable ones), see OpenVicEconomyDefines
		struct supply_item_t {
			GoodDefinition const* good;
			fixed_point_t construction_amount; // The amount of the good in the construction cost of the unit
			bool consumable; // A good that is used up (food, fuel, coal) rather than worn out
			bool engine_fuel; // Coal and fuel: a ship burns them only when it is not in a port
		};

	private:
		std::vector<supply_item_t> PROPERTY(supply_items);

		terrain_modifiers_t PROPERTY(terrain_modifiers);

	protected:
		/* Non-const reference unit_args so variables can be moved from it. */
		UnitType(
			std::string_view new_identifier, unit_type_index_t new_unit_type_index, unit_branch_t new_branch,
			unit_type_args_t& unit_args
		);

	public:
		const unit_type_index_t unit_type_index; // Position in the combined unit type registry
		const unit_branch_t branch; /* type in defines */
		const unit_category_t unit_category;
		const bool starts_unlocked;
		const bool has_floating_flag;
		const icon_t icon;
		const uint32_t priority;
		const fixed_point_t max_strength;
		const fixed_point_t default_organisation;
		const fixed_point_t maximum_speed;
		const fixed_point_t weighted_value;
		const Timespan build_time;
		const fixed_point_t supply_consumption;

		UnitType(UnitType&&) = default;
	};

	template<>
	struct UnitTypeBranched<unit_branch_t::LAND> : UnitType, HasIndex<RegimentType, regiment_type_index_t> {
		static constexpr regiment_allowed_cultures_t allowed_cultures_get_most_permissive(
			regiment_allowed_cultures_t lhs, regiment_allowed_cultures_t rhs
		) {
			return std::min(lhs, rhs);
		}

		struct regiment_type_args_t {
			regiment_allowed_cultures_t allowed_cultures = regiment_allowed_cultures_t::ALL_CULTURES;
			std::string_view sprite_override, sprite_mount, sprite_mount_attach_node;
			// TODO - represent these as modifier effects, so that they can be combined with tech, inventions,
			// leader bonuses, etc. and applied to unit instances all in one go (same for ShipTypes below)
			fixed_point_t reconnaissance = 0, attack = 0, defence = 0, discipline = 0, support = 0, maneuver = 0,
				siege = 0;

			constexpr regiment_type_args_t() {};
			regiment_type_args_t(regiment_type_args_t&&) = default;
		};

	private:
		memory::string PROPERTY(sprite_override);
		memory::string PROPERTY(sprite_mount);
		memory::string PROPERTY(sprite_mount_attach_node);

	public:
		const regiment_allowed_cultures_t allowed_cultures;
		const fixed_point_t reconnaissance;
		const fixed_point_t attack;
		const fixed_point_t defence;
		const fixed_point_t discipline;
		const fixed_point_t support;
		const fixed_point_t maneuver;
		const fixed_point_t siege;

		// A buff of a unit type: shares added to the attack, the defence and the breakthrough
		struct OvBuff {
			fixed_point_t attack_mult;
			fixed_point_t defense_mult;
			fixed_point_t breakthrough_mult;
		};
		// What a unit type of OpenVic (ov_data/common/ov_units) has besides the stats of the game; the types of Vic2 have the defaults (is_ov = false)
		struct OvUnitData {
			bool is_ov = false;
			memory::string unlock_key; // empty: open from the start
			memory::vector<memory::string> equipment; // the weapons the type may have (their ids); the lowest generation is the one it is built with
			memory::string category_scope; // infantry, cavalry or artillery
			fixed_point_t breakthrough;
			fixed_point_t weight; // kilograms of a regiment, 0: made from the strength
			fixed_point_t occupation_weight = fixed_point_t::_1;
			memory::vector<std::pair<memory::string, fixed_point_t>> scope_weights; // the category scopes with weights (empty: the category of the type, weight 1)
			memory::string upgrades_to;
			fixed_point_map_t<GoodDefinition const*> upgrade_cost;
			OvBuff general;
			memory::vector<std::pair<memory::string, OvBuff>> vs_unit; // against a type of units or a category
			memory::vector<std::pair<memory::string, OvBuff>> terrain; // on a terrain
		};
		OvUnitData ov;

		[[nodiscard]] fixed_point_t get_breakthrough() const {
			return ov.is_ov ? ov.breakthrough : attack;
		}

		UnitTypeBranched(
			index_t new_index, unit_type_index_t new_unit_type_index, std::string_view new_identifier,
			unit_type_args_t& unit_args, regiment_type_args_t const& regiment_type_args
		);
		UnitTypeBranched(UnitTypeBranched&&) = default;
	};

	template<>
	struct UnitTypeBranched<unit_branch_t::NAVAL> : UnitType, HasIndex<ShipType, ship_type_index_t> {
		struct ship_type_args_t {
			icon_t naval_icon = 0;
			bool sail = false, transport = false, capital = false, build_overseas = false;
			uint32_t min_port_level = 0;
			int32_t limit_per_port = 0;
			fixed_point_t colonial_points = 0, supply_consumption_score = 0, hull = 0, gun_power = 0, fire_range = 0,
				evasion = 0, torpedo_attack = 0;

			constexpr ship_type_args_t() {};
			ship_type_args_t(ship_type_args_t&&) = default;
		};

	public:
		const bool can_sail;
		const bool is_transport;
		const bool is_capital;
		const bool can_build_overseas;

		const icon_t naval_icon;
		const fixed_point_t colonial_points;
		const uint32_t min_port_level;
		const int32_t limit_per_port;
		const fixed_point_t supply_consumption_score;

		const fixed_point_t hull;
		const fixed_point_t gun_power;
		const fixed_point_t fire_range;
		const fixed_point_t evasion;
		const fixed_point_t torpedo_attack;

		UnitTypeBranched(
			index_t new_index, unit_type_index_t new_unit_type_index, std::string_view new_identifier,
			unit_type_args_t& unit_args, ship_type_args_t const& ship_type_args
		);
		UnitTypeBranched(UnitTypeBranched&&) = default;
	};

	struct UnitTypeManager {
	private:
		IdentifierPointerRegistry<UnitType> IDENTIFIER_REGISTRY(unit_type);
		IdentifierRegistry<RegimentType> IDENTIFIER_REGISTRY(regiment_type);
		IdentifierRegistry<ShipType> IDENTIFIER_REGISTRY(ship_type);
		string_map_t<memory::string> vic2_aliases;

	public:
		void reserve_all_unit_types(size_t size);
		void lock_all_unit_types();

		bool add_regiment_type(
			std::string_view identifier, UnitType::unit_type_args_t& unit_args,
			RegimentType::regiment_type_args_t const& regiment_type_args
		);
		bool add_ship_type(
			std::string_view identifier, UnitType::unit_type_args_t& unit_args,
			ShipType::ship_type_args_t const& ship_type_args
		);

		static NodeTools::Callback<std::string_view> auto expect_branch_str(
			NodeTools::Callback<unit_branch_t> auto callback
		) {
			using enum unit_branch_t;
			static const string_map_t<unit_branch_t> branch_map {
				{ "land", LAND }, { "naval", NAVAL }, { "sea", NAVAL }
			};
			return NodeTools::expect_mapped_string(branch_map, callback);
		}
		static NodeTools::NodeCallback auto expect_branch_identifier(NodeTools::Callback<unit_branch_t> auto callback) {
			return NodeTools::expect_identifier(expect_branch_str(callback));
		}

		constexpr NodeTools::NodeCallback auto expect_unit_type_index(
			NodeTools::Callback<unit_type_index_t> auto callback, bool warn = false
		) const {
			return expect_unit_type_identifier(
				[callback](UnitType const& unit_type) -> bool {
					return callback(unit_type.unit_type_index);
				},
				warn
			);
		}
		constexpr NodeTools::Callback<std::string_view> auto expect_unit_type_index_str(
			NodeTools::Callback<unit_type_index_t> auto callback, bool allow_empty = false, bool warn = false
		) const {
			return expect_unit_type_str(
				[callback](UnitType const& unit_type) -> bool {
					return callback(unit_type.unit_type_index);
				},
				allow_empty, warn
			);
		}

		// The types of units of OpenVic, a file of ov_data/common/ov_units each (the type of Vic2 with the same id is not loaded): the number of types loaded
		std::size_t load_ov_unit_file(
			GoodDefinitionManager const& good_definition_manager, EquipmentManager const& equipment_manager, std::string_view source, ast::NodeCPtr root
		);
		// The ids of the units of Vic2 that are other units here (infantry is line_infantry): ov_data/common/ov_units/aliases.txt. Only the files of
		// Vic2 (the armies of the start) use them; resolve_vic2_regiment_type gives the type
		bool load_vic2_aliases(std::string_view source, ast::NodeCPtr root);
		[[nodiscard]] RegimentType const* resolve_vic2_regiment_type(std::string_view identifier) const;
		[[nodiscard]] bool is_vic2_alias(std::string_view identifier) const {
			return vic2_aliases.contains(identifier);
		}

		bool load_unit_type_file(
			GoodDefinitionManager const& good_definition_manager, TerrainTypeManager const& terrain_type_manager,
			ModifierManager const& modifier_manager, ovdl::v2script::Parser const& parser
		);
		bool generate_modifiers(ModifierManager& modifier_manager) const;
	};
}

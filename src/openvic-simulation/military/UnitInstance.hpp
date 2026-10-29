#pragma once

#include <string_view>

#include "openvic-simulation/core/object/Date.hpp"
#include "openvic-simulation/core/object/FixedPoint.hpp"
#include "openvic-simulation/core/object/Timespan.hpp"
#include "openvic-simulation/military/UnitType.hpp"
#include "openvic-simulation/types/UnitBranchType.hpp"
#include "openvic-simulation/types/UniqueId.hpp"
#include "openvic-simulation/utility/Getters.hpp"

namespace OpenVic {
	struct EquipmentDefinition;
	struct GoodDefinition;

	struct UnitInstance {
	private:
		memory::string PROPERTY(name);
		fixed_point_t PROPERTY(organisation);
		fixed_point_t PROPERTY(max_organisation);
		fixed_point_t PROPERTY(strength);
		// The reserve of supplies of the unit in days (docs/PLAN_ARMY_SUPPLY_FRONTS.md): it is used while the supplies do not arrive, and is
		// filled while they do. Negative at first: on the first day it is full (the units of the start and the new ones come supplied)
		fixed_point_t PROPERTY_RW(supply_reserve, fixed_point_t(-1));

	protected:
		UnitInstance(
			unique_id_t new_unique_id,
			std::string_view new_name,
			UnitType const& new_unit_type
		);

	public:
		const unique_id_t unique_id;
		UnitType const& unit_type;

		UnitInstance(UnitInstance&&) = default;

		inline constexpr fixed_point_t get_max_strength() const {
			return unit_type.max_strength;
		}

		inline constexpr unit_branch_t get_branch() const {
			return unit_type.branch;
		}

		void set_name(std::string_view new_name);

		// Combat helpers, values are clamped to [0, max]
		void take_damage(fixed_point_t organisation_loss, fixed_point_t strength_loss);
		void recover(fixed_point_t organisation_gain, fixed_point_t strength_gain);

		constexpr bool operator==(UnitInstance const& rhs) const {
			return unique_id == rhs.unique_id;
		}
	};

	struct Pop;

	template<>
	struct UnitInstanceBranched<unit_branch_t::LAND> : UnitInstance {
		friend struct UnitInstanceManager;

	private:
		Pop* PROPERTY_PTR(pop);
		bool PROPERTY_CUSTOM_PREFIX(mobilised, is);
		// The division of its army (DivisionInstance::id) the regiment is in, 0 if it is in none (a country without divisions)
		std::uint32_t PROPERTY_RW(division_id, 0);
		// The weapon of the regiment (military/Equipment.hpp): nullptr while it has the first generation of its line (the arms it was made with).
		// weapon_base_good is the good of that first generation (the one its supply items name), weapon_good the good of the weapon it has; the ratios
		// are what its attack and its defence are worth in the battle (1 with the first generation), worked out when the weapon changes
		EquipmentDefinition const* PROPERTY_RW(weapon, nullptr);
		GoodDefinition const* PROPERTY_RW(weapon_base_good, nullptr);
		GoodDefinition const* PROPERTY_RW(weapon_good, nullptr);
		fixed_point_t PROPERTY_RW(weapon_attack_ratio, fixed_point_t::_1);
		fixed_point_t PROPERTY_RW(weapon_defence_ratio, fixed_point_t::_1);

		UnitInstanceBranched(
			unique_id_t new_unique_id,
			std::string_view new_name,
			RegimentType const& new_regiment_type,
			Pop* new_pop,
			bool new_mobilised
		);

	public:
		UnitInstanceBranched(UnitInstanceBranched&&) = default;

		constexpr RegimentType const& get_regiment_type() const {
			return static_cast<RegimentType const&>(unit_type);
		}
	};

	template<>
	struct UnitInstanceBranched<unit_branch_t::NAVAL> : UnitInstance {
		friend struct UnitInstanceManager;

	private:
		UnitInstanceBranched(
			unique_id_t new_unique_id,
			std::string_view new_name,
			ShipType const& new_ship_type
		);

	public:
		UnitInstanceBranched(UnitInstanceBranched&&) = default;

		constexpr ShipType const& get_ship_type() const {
			return static_cast<ShipType const&>(unit_type);
		}
	};
}

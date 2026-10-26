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

	struct UnitInstance {
	private:
		memory::string PROPERTY(name);
		fixed_point_t PROPERTY(organisation);
		fixed_point_t PROPERTY(max_organisation);
		fixed_point_t PROPERTY(strength);
		// The day of the month the unit was created on: once a month on this day it uses its supplies
		int32_t PROPERTY_RW(supply_day, 1);

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

		// The supplies are used once a month, on the day of the month the unit was created on (on the last day of a shorter month)
		bool is_supply_day(const Date today) const {
			const int32_t day = static_cast<int32_t>(today.get_day());
			return day == supply_day || (supply_day > day && (today + Timespan::from_days(1)).is_month_start());
		}

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

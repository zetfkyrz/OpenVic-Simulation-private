#pragma once

#include <string_view>
#include <utility>

#include "openvic-simulation/core/object/Date.hpp"
#include "openvic-simulation/core/memory/Vector.hpp"
#include "openvic-simulation/core/object/FixedPoint.hpp"
#include "openvic-simulation/types/UnitBranchType.hpp"
#include "openvic-simulation/types/UniqueId.hpp"
#include "openvic-simulation/utility/Getters.hpp"

namespace OpenVic {
	struct DeploymentManager;
	struct UnitInstanceManager;
	struct LeaderTrait;
	struct TerrainType;

	struct LeaderBase {
		friend struct DeploymentManager;
		friend struct UnitInstanceManager;

	private:
		memory::string PROPERTY(name);
		LeaderTrait const* PROPERTY(personality);
		LeaderTrait const* PROPERTY(background);
		fixed_point_t PROPERTY(prestige);
		memory::string PROPERTY(picture);

	protected:
		LeaderBase(LeaderBase const&) = default;

	public:
		const unit_branch_t branch; /* type in defines */
		const Date date;

		LeaderBase(
			std::string_view new_name, unit_branch_t new_branch, Date new_date, LeaderTrait const* new_personality,
			LeaderTrait const* new_background, fixed_point_t new_prestige, std::string_view new_picture
		);
		LeaderBase(LeaderBase&&) = default;

		void set_picture(std::string_view new_picture);
		// Prestige never goes below zero
		void add_prestige(const fixed_point_t amount) {
			prestige += amount;
			if (prestige < fixed_point_t::_0) {
				prestige = fixed_point_t::_0;
			}
		}
	};

	struct UnitInstanceManager;
	struct UnitInstanceGroup;
	struct CountryInstance;

	struct LeaderInstance : LeaderBase {

		friend struct UnitInstanceManager;
		friend struct UnitInstanceGroup;

	private:
		UnitInstanceGroup* PROPERTY_PTR(unit_instance_group, nullptr);
		bool PROPERTY_RW(can_be_used, true);
		// The general is the field marshal of a group of armies (he does not lead an army)
		bool PROPERTY_RW_CUSTOM_NAME(marshal, is_marshal, set_marshal, false);
		// The experience (docs/WAR_ASSUMPTIONS.md, section 7, military/LeaderSkills.hpp): of a general in the battles of his army (the attack, the
		// defence, every terrain), of a field marshal in the wars of his group (the supply, the planning). Both are kept when the role changes
		fixed_point_t PROPERTY_RW(attack_experience, fixed_point_t::_0);
		fixed_point_t PROPERTY_RW(defence_experience, fixed_point_t::_0);
		fixed_point_t PROPERTY_RW(supply_experience, fixed_point_t::_0);
		fixed_point_t PROPERTY_RW(planning_experience, fixed_point_t::_0);
		// The terrains in the order they were first fought in
		memory::vector<std::pair<TerrainType const*, fixed_point_t>> PROPERTY(terrain_experience);
	public:
		const unique_id_t unique_id;

		CountryInstance const& country;

		LeaderInstance(unique_id_t new_unique_id, LeaderBase const& leader_base, CountryInstance const& new_country);

		[[nodiscard]] fixed_point_t get_terrain_experience_of(TerrainType const& terrain) const {
			for (auto const& [known, experience] : terrain_experience) {
				if (known == &terrain) {
					return experience;
				}
			}
			return fixed_point_t::_0;
		}
		void add_terrain_experience(TerrainType const& terrain, const fixed_point_t amount) {
			for (auto& [known, experience] : terrain_experience) {
				if (known == &terrain) {
					experience += amount;
					return;
				}
			}
			terrain_experience.emplace_back(&terrain, amount);
		}

		constexpr bool operator==(LeaderInstance const& rhs) const {
			return unique_id == rhs.unique_id;
		}
	};
}

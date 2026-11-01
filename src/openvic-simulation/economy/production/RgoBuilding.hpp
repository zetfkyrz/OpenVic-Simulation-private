#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "openvic-simulation/core/memory/String.hpp"
#include "openvic-simulation/core/memory/Vector.hpp"
#include "openvic-simulation/core/object/FixedPoint.hpp"
#include "openvic-simulation/dataloader/NodeTools.hpp"

namespace OpenVic {
	struct CountryInstance;
	struct GoodDefinition;
	struct GoodDefinitionManager;
	struct InstanceManager;
	struct OpenVicEconomyDefines;
	struct ProvinceInstance;

	// The buildings of the resource gathering operations (docs/RGO_BUILDINGS.md): ov_data/common/ov_rgo_buildings/*.txt
	//
	//   fishing_harbour = { goods = { fish } }                                 # the building of the RGOs of these goods
	//   farm = { goods = { cattle wool silk } max_level = 2 build_days = 90 lumber = 10 }   # the last three are optional (ov_economy RGO_BUILDING_*)
	//
	// A level gives RGO_BUILDING_STAFF_PER_LEVEL (+5 %) more workplaces and RGO_BUILDING_OUTPUT_PER_LEVEL (+5 %) more output to the RGO of the province.
	// The levels above the maximum come with the unlock keys "rgo_level:<id>:<level>" of the technologies (the unlock layer).
	struct RgoBuildingType {
		memory::string identifier;
		memory::vector<GoodDefinition const*> goods;
		std::int32_t max_level = -1; // -1: RGO_BUILDING_MAX_LEVEL
		std::int32_t build_days = -1; // -1: RGO_BUILDING_BUILD_DAYS
		fixed_point_t lumber = fixed_point_t::_0; // 0: RGO_BUILDING_LUMBER
	};

	enum struct rgo_build_result_t { OK, DISABLED, NO_RGO, NO_BUILDING_TYPE, NOT_OWNED, BUSY, MAX_LEVEL, NO_LUMBER, NO_MONEY };
	// Who pays: the pops of the province pay the price of the lumber to the treasury (the country gives the lumber out of its stockpile), or the state pays
	// (the lumber out of the stockpile, which the treasury has bought)
	enum struct rgo_builder_t { POPS, STATE };

	struct RgoBuildingData {
	private:
		memory::vector<RgoBuildingType> types;

	public:
		std::size_t load_file(std::string_view source, ast::NodeCPtr root, GoodDefinitionManager const& good_manager);
		[[nodiscard]] std::span<const RgoBuildingType> get_types() const {
			return types;
		}
		[[nodiscard]] RgoBuildingType const* get_type_for_good(GoodDefinition const& good) const;

		[[nodiscard]] static std::int32_t get_max_level(RgoBuildingType const& type, OpenVicEconomyDefines const& defines, CountryInstance const& owner);
		[[nodiscard]] static std::int32_t get_build_days(RgoBuildingType const& type, OpenVicEconomyDefines const& defines);
		[[nodiscard]] static fixed_point_t get_lumber(RgoBuildingType const& type, OpenVicEconomyDefines const& defines);

		// Once a month: the pops of every country start a level where there are more workers than workplaces (they pay for the lumber), and the AI of a
		// country pays for it where the pops are too poor
		static void monthly_update(InstanceManager& instance_manager);
		// Starts the next level in the province (the main thread, between ticks)
		[[nodiscard]] static rgo_build_result_t start_building(InstanceManager& instance_manager, ProvinceInstance& province, rgo_builder_t builder);
	};
}

#include "RgoBuilding.hpp"

#include <algorithm>
#include <fstream>
#include <string>

#include "openvic-simulation/DefinitionManager.hpp"
#include "openvic-simulation/InstanceManager.hpp"
#include "openvic-simulation/country/CountryInstance.hpp"
#include "openvic-simulation/defines/OpenVicDefines.hpp"
#include "openvic-simulation/economy/GoodDefinition.hpp"
#include "openvic-simulation/economy/GoodInstance.hpp"
#include "openvic-simulation/economy/production/ProductionType.hpp"
#include "openvic-simulation/economy/production/ResourceGatheringOperation.hpp"
#include "openvic-simulation/map/ProvinceInstance.hpp"
#include "openvic-simulation/population/Pop.hpp"
#include "openvic-simulation/utility/Logger.hpp"

using namespace OpenVic;
using namespace OpenVic::NodeTools;

std::size_t RgoBuildingData::load_file(std::string_view source, ast::NodeCPtr root, GoodDefinitionManager const& good_manager) {
	std::size_t loaded = 0;
	const bool parsed = expect_dictionary([this, source, &good_manager, &loaded](std::string_view key, ast::NodeCPtr value) -> bool {
		RgoBuildingType type;
		type.identifier = key;
		fixed_point_t lumber = fixed_point_t::_0;
		bool has_lumber = false;
		const bool ok = expect_dictionary_keys_and_default(
			[source, key](std::string_view inner, ast::NodeCPtr) -> bool {
				spdlog::warn_s("Unknown key \"{}\" in the RGO building {} ({})", inner, key, source);
				return true;
			},
			"goods", ONE_EXACTLY, expect_list(
				expect_identifier_or_string([&type, &good_manager, key, source](std::string_view good_identifier) -> bool {
					GoodDefinition const* const good = good_manager.get_good_definition_by_identifier(good_identifier);
					if (good == nullptr) {
						spdlog::warn_s("The good \"{}\" of the RGO building {} does not exist ({})", good_identifier, key, source);
						return true;
					}
					type.goods.push_back(good);
					return true;
				})
			),
			"max_level", ZERO_OR_ONE, expect_int<std::int32_t>([&type](std::int32_t level) -> bool {
				type.max_level = level;
				return level >= 0;
			}),
			"build_days", ZERO_OR_ONE, expect_int<std::int32_t>([&type](std::int32_t days) -> bool {
				type.build_days = days;
				return days >= 1;
			}),
			"lumber", ZERO_OR_ONE, expect_fixed_point([&lumber, &has_lumber](fixed_point_t amount) -> bool {
				lumber = amount;
				has_lumber = amount >= fixed_point_t::_0;
				return has_lumber;
			})
		)(value);
		if (has_lumber) {
			type.lumber = lumber;
		}
		if (!ok || type.goods.empty()) {
			spdlog::warn_s("The RGO building {} is skipped: it has errors or no goods ({})", key, source);
			return true;
		}
		// A later file (a mod) with the same identifier replaces the building
		for (RgoBuildingType& existing : types) {
			if (existing.identifier == type.identifier) {
				existing = std::move(type);
				loaded++;
				return true;
			}
		}
		types.push_back(std::move(type));
		loaded++;
		return true;
	})(root);
	if (!parsed) {
		spdlog::warn_s("The RGO buildings file {} is broken, what could be read is used", source);
	}
	return loaded;
}

RgoBuildingType const* RgoBuildingData::get_type_for_good(GoodDefinition const& good) const {
	// The last building that names the good wins (a mod can move a good to another building)
	RgoBuildingType const* found = nullptr;
	for (RgoBuildingType const& type : types) {
		for (GoodDefinition const* const type_good : type.goods) {
			if (type_good == &good) {
				found = &type;
			}
		}
	}
	return found;
}

std::int32_t RgoBuildingData::get_max_level(RgoBuildingType const& type, OpenVicEconomyDefines const& defines, CountryInstance const& owner) {
	std::int32_t level = type.max_level >= 0 ? type.max_level : defines.rgo_building_max_level;
	// The technologies open the next levels one by one
	for (int extra = 0; extra < 16; ++extra) {
		const std::string key = "rgo_level:" + std::string { type.identifier } + ":" + std::to_string(level + 1);
		if (!owner.has_unlock(key)) {
			break;
		}
		level++;
	}
	return level;
}

std::int32_t RgoBuildingData::get_build_days(RgoBuildingType const& type, OpenVicEconomyDefines const& defines) {
	return type.build_days >= 1 ? type.build_days : defines.rgo_building_build_days;
}

fixed_point_t RgoBuildingData::get_lumber(RgoBuildingType const& type, OpenVicEconomyDefines const& defines) {
	return type.lumber > fixed_point_t::_0 ? type.lumber : defines.rgo_building_lumber;
}

rgo_build_result_t RgoBuildingData::start_building(InstanceManager& instance_manager, ProvinceInstance& province, const rgo_builder_t builder) {
	DefinitionManager const& definitions = instance_manager.definition_manager;
	OpenVicEconomyDefines const& defines = definitions.get_define_manager().get_openvic_defines().economy;
	if (defines.rgo_building_enabled == 0) {
		return rgo_build_result_t::DISABLED;
	}
	ResourceGatheringOperation& rgo = province.get_mutable_rgo();
	if (!rgo.is_valid()) {
		return rgo_build_result_t::NO_RGO;
	}
	CountryInstance* const owner = province.get_owner();
	if (owner == nullptr) {
		return rgo_build_result_t::NOT_OWNED;
	}
	RgoBuildingType const* const type = definitions.get_economy_manager().get_production_type_manager().get_rgo_buildings().get_type_for_good(
		rgo.get_production_type_nullable()->output_good
	);
	if (type == nullptr) {
		return rgo_build_result_t::NO_BUILDING_TYPE;
	}
	if (rgo.get_building_days_left() > 0) {
		return rgo_build_result_t::BUSY;
	}
	if (rgo.get_building_level() >= get_max_level(*type, defines, *owner)) {
		return rgo_build_result_t::MAX_LEVEL;
	}

	GoodDefinition const* const lumber_good = definitions.get_economy_manager().get_good_definition_manager().get_good_definition_by_identifier("lumber");
	const fixed_point_t lumber = get_lumber(*type, defines);
	if (lumber_good != nullptr && lumber > fixed_point_t::_0) {
		CountryInstance::good_data_t& good_data = owner->get_good_data(*lumber_good);
		if (good_data.stockpile_amount < lumber) {
			return rgo_build_result_t::NO_LUMBER;
		}
		if (builder == rgo_builder_t::POPS) {
			// The pops of the province pay the price of the lumber to the treasury, every pop its share of the cash
			const fixed_point_t price = instance_manager.get_good_instance_manager().get_good_instance_by_definition(*lumber_good).get_price() * lumber;
			fixed_point_t cash = fixed_point_t::_0;
			for (Pop const& pop : province.get_pops()) {
				cash += pop.get_cash().get_copy_of_value();
			}
			if (cash < price) {
				return rgo_build_result_t::NO_MONEY;
			}
			fixed_point_t left = price;
			for (Pop& pop : province.get_mutable_pops()) {
				const fixed_point_t share = std::min(left, fp::mul_div(price, pop.get_cash().get_copy_of_value(), cash));
				left -= pop.take_cash(share);
			}
			// The rounding rest comes from the first pops that have the cash
			for (Pop& pop : province.get_mutable_pops()) {
				if (left <= fixed_point_t::_0) {
					break;
				}
				left -= pop.take_cash(left);
			}
			owner->add_cash(price - left);
		}
		good_data.stockpile_amount -= lumber;
		good_data.stockpile_change_yesterday -= lumber;
	}
	rgo.begin_building(get_build_days(*type, defines));
	return rgo_build_result_t::OK;
}

void RgoBuildingData::monthly_update(InstanceManager& instance_manager) {
	OpenVicEconomyDefines const& defines = instance_manager.definition_manager.get_define_manager().get_openvic_defines().economy;
	if (defines.rgo_building_enabled == 0 || defines.rgo_building_starts_per_month <= 0) {
		return;
	}
	for (CountryInstance& country : instance_manager.get_country_instance_manager().get_country_instances()) {
		if (!country.exists() || country.is_rebel_country() || country.get_owned_provinces().empty()) {
			continue;
		}
		std::int32_t starts = 0;
		for (ProvinceInstance* const province : country.get_owned_provinces()) {
			if (starts >= defines.rgo_building_starts_per_month) {
				break;
			}
			ResourceGatheringOperation const& rgo = province->get_rgo();
			if (!rgo.is_valid() || rgo.get_building_days_left() > 0) {
				continue;
			}
			// An RGO that sells its output is worth a level (+staff, +output for ~10 lumber); the unemployed take the new places
			if (rgo.get_revenue_yesterday() <= fixed_point_t::_0) {
				continue;
			}
			rgo_build_result_t result = start_building(instance_manager, *province, rgo_builder_t::POPS);
			if (result == rgo_build_result_t::NO_MONEY && country.is_ai()) {
				result = start_building(instance_manager, *province, rgo_builder_t::STATE);
			}
			if (result == rgo_build_result_t::OK) {
				starts++;
			} else if (result == rgo_build_result_t::NO_LUMBER) {
				break; // the stockpile has no more lumber this month
			}
		}
	}
}

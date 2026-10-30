#include "DivisionStats.hpp"

#include <algorithm>
#include <map>
#include <string>

#include "openvic-simulation/country/CountryInstance.hpp"
#include "openvic-simulation/DefinitionManager.hpp"
#include "openvic-simulation/economy/GoodDefinition.hpp"
#include "openvic-simulation/InstanceManager.hpp"
#include "openvic-simulation/military/BattleFormulas.hpp"
#include "openvic-simulation/military/Equipment.hpp"
#include "openvic-simulation/military/UnitType.hpp"
#include "openvic-simulation/military/Unlocks.hpp"
#include "openvic-simulation/modifier/ModifierEffectCache.hpp"
#include "openvic-simulation/modifier/ModifierManager.hpp"

using namespace OpenVic;

DivisionStats OpenVic::compute_division_stats(
	InstanceManager const& instance_manager, CountryInstance const& country, DivisionTemplate const& division_template,
	const fixed_point_t equipment_fill
) {
	DefinitionManager const& definitions = instance_manager.definition_manager;
	OpenVicDefines const& defines = definitions.get_define_manager().get_openvic_defines();
	UnitTypeManager const& unit_type_manager = definitions.get_military_manager().get_unit_type_manager();
	EquipmentManager const& equipment_manager = definitions.get_military_manager().get_equipment_manager();
	ModifierEffectCache const& cache = definitions.get_modifier_manager().get_modifier_effect_cache();
	const DivisionTemplateLimits limits = get_template_limits(instance_manager, country);

	const auto effect_value = [&country](ModifierEffect const* effect) -> fixed_point_t {
		return effect != nullptr ? country.get_modifier_effect_value(*effect) : fixed_point_t::_0;
	};

	DivisionStats stats;
	stats.reliability = std::clamp(equipment_fill, fixed_point_t::_0, fixed_point_t::_1);

	// The line of weapons of every regiment type, and the weapon of it the country has of the highest generation
	struct type_info_t {
		bool looked_up = false;
		EquipmentDefinition const* first = nullptr;
		EquipmentDefinition const* best = nullptr;
	};
	std::array<type_info_t, DIVISION_MAX_TYPES> type_infos;
	const auto info_of = [&](RegimentType const& regiment_type, const std::size_t kind) -> type_info_t const& {
		type_info_t& info = type_infos[kind];
		if (!info.looked_up) {
			info.looked_up = true;
			for (EquipmentLine const& line : equipment_manager.get_lines()) {
				if (std::find(line.unit_types.begin(), line.unit_types.end(), regiment_type.get_identifier()) == line.unit_types.end()) {
					continue;
				}
				const auto generations = equipment_manager.get_line_equipment(line.identifier);
				if (!generations.empty()) {
					info.first = equipment_manager.get_initial_equipment(line.identifier);
					info.best = get_best_equipment(equipment_manager, country, line.identifier);
				}
				break;
			}
		}
		return info;
	};

	std::map<std::string, fixed_point_t> cost, supply, ammo;
	std::array<std::uint32_t, DIVISION_MAX_RANKS> cells_in_rank {};
	fixed_point_t strength_sum = fixed_point_t::_0, organisation_sum = fixed_point_t::_0, durability_sum = fixed_point_t::_0;
	fixed_point_t speed = fixed_point_t::_0;

	for (std::size_t rank = 0; rank < DIVISION_MAX_RANKS; ++rank) {
		for (std::size_t row = 0; row < DIVISION_MAX_DEPTH; ++row) {
			const std::uint8_t cell = division_template.cells[DivisionTemplate::cell_index(rank, row)];
			if (cell == 0) {
				continue;
			}
			RegimentType const* const regiment_type = unit_type_manager.get_regiment_type_by_index(regiment_type_index_t(cell - 1));
			if (regiment_type == nullptr) {
				continue;
			}
			type_info_t const& info = info_of(*regiment_type, cell - 1);
			const bool has_weapon = info.first != nullptr && info.best != nullptr;
			const fixed_point_t attack_ratio = has_weapon
				? battle_formulas::weapon_ratio(info.first->attack_mult, info.best->attack_mult, equipment_fill) : fixed_point_t::_1;
			const fixed_point_t defence_ratio = has_weapon
				? battle_formulas::weapon_ratio(info.first->defense_mult, info.best->defense_mult, equipment_fill) : fixed_point_t::_1;
			const fixed_point_t row_weight = battle_formulas::row_weight(row, limits.row_weight_shift);

			stats.regiments++;
			cells_in_rank[rank]++;
			stats.attack += regiment_type->attack * attack_ratio * row_weight;
			stats.defence += regiment_type->defence * defence_ratio * row_weight;
			durability_sum += regiment_type->max_strength * defence_ratio;
			strength_sum += regiment_type->max_strength;
			organisation_sum += regiment_type->default_organisation;
			stats.reconnaissance += regiment_type->reconnaissance;
			speed = stats.regiments == 1 ? regiment_type->maximum_speed : std::min(speed, regiment_type->maximum_speed);
			stats.weight += regiment_type->max_strength * defines.war.template_kit_weight_per_strength;

			// The goods: the weapon of the line is the one of the best generation (the good of the first generation is the one of the base game)
			const std::string base_good = has_weapon
				? std::string { info.first->good.identifier }
				: std::string {};
			const auto good_id = [&](GoodDefinition const& good) -> std::string {
				if (has_weapon && good.get_identifier() == base_good) {
					return std::string { info.best->good.identifier };
				}
				return std::string { good.get_identifier() };
			};
			fixed_point_t pieces = fixed_point_t::_0;
			for (auto const& [good, quantity] : regiment_type->get_build_cost()) {
				cost[good_id(*good)] += quantity;
				if (has_weapon && good->get_identifier() == base_good) {
					pieces += quantity;
				}
			}
			for (UnitType::supply_item_t const& item : regiment_type->get_supply_items()) {
				OpenVicEconomyDefines const& economy = defines.economy;
				const fixed_point_t share_of = item.consumable ? economy.army_supply_consumable_share.of(item.construction_amount)
					: economy.army_supply_durable_share.of(item.construction_amount);
				supply[good_id(*item.good)] += share_of * regiment_type->supply_consumption / economy.supply_days_per_month;
			}
			if (has_weapon) {
				stats.weight += pieces * info.best->weight;
				ammo[info.best->ammo_good.empty() ? std::string { "ammunition" } : std::string { info.best->ammo_good }] +=
					pieces * info.best->ammo_per_day;
			}
		}
	}

	for (const std::uint32_t cells : cells_in_rank) {
		stats.width += cells > 0;
	}
	if (stats.regiments > 0) {
		stats.depth = fixed_point_t(static_cast<std::int32_t>(stats.regiments)) / static_cast<std::int32_t>(stats.width);
		stats.organisation = organisation_sum / static_cast<std::int32_t>(stats.regiments);
	}
	stats.speed = speed;
	stats.durability = durability_sum * (fixed_point_t::_1 + effect_value(cache.get_division_durability()));
	stats.density = battle_formulas::density(strength_sum, stats.width, limits.density_reduction);
	stats.fortification = effect_value(cache.get_fortification());
	stats.reconnaissance += effect_value(cache.get_division_reconnaissance());
	stats.occupation_speed = defines.war.occupation_speed_base + effect_value(cache.get_occupation_speed());
	stats.occupation_instant = stats.occupation_speed * 100 >= fixed_point_t(defines.war.template_occupation_instant_percent);
	stats.men = strength_sum * defines.war.template_men_per_strength;

	const auto to_vector = [](std::map<std::string, fixed_point_t> const& map, memory::vector<DivisionGoodAmount>& out) {
		for (auto const& [good, amount] : map) {
			out.push_back({ memory::string { good }, amount });
		}
	};
	to_vector(cost, stats.cost);
	to_vector(supply, stats.supply_per_day);
	to_vector(ammo, stats.ammo_per_day);
	return stats;
}

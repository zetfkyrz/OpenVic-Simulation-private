#include "DivisionStats.hpp"

#include <algorithm>
#include <map>
#include <string>

#include "openvic-simulation/country/CountryInstance.hpp"
#include "openvic-simulation/DefinitionManager.hpp"
#include "openvic-simulation/economy/GoodDefinition.hpp"
#include "openvic-simulation/InstanceManager.hpp"
#include "openvic-simulation/military/BattleFormulas.hpp"
#include "openvic-simulation/military/Companies.hpp"
#include "openvic-simulation/military/Equipment.hpp"
#include "openvic-simulation/military/UnitType.hpp"
#include "openvic-simulation/military/Unlocks.hpp"
#include "openvic-simulation/military/UnitEquipment.hpp"
#include "openvic-simulation/military/UnitModifiers.hpp"
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

	// The weapons of every regiment type: what it may have and what the country gives it (military/UnitEquipment.hpp)
	struct type_info_t {
		bool looked_up = false;
		UnitWeapon weapon;
	};
	std::array<type_info_t, DIVISION_MAX_TYPES> type_infos;
	const fixed_point_t below_min_penalty = defines.war.equipment_below_min_penalty.fixed();
	const auto info_of = [&](RegimentType const& regiment_type, const std::size_t kind) -> type_info_t const& {
		type_info_t& info = type_infos[kind];
		if (!info.looked_up) {
			info.looked_up = true;
			info.weapon = choose_unit_weapon(
				equipment_manager, country, regiment_type, get_allowed_weapons(equipment_manager, regiment_type), below_min_penalty
			);
		}
		return info;
	};

	std::map<std::string, fixed_point_t> cost, supply, ammo;
	// The support companies of the template: how many of every type, the width they are shared over, what they work as today (the country)
	CompanyTypeManager const& company_types = definitions.get_military_manager().get_company_type_manager();
	const memory::vector<std::uint32_t> company_counts = count_template_companies(company_types, division_template);
	const std::uint32_t company_width = get_template_width(division_template);
	fixed_point_t company_men = fixed_point_t::_0;
	fixed_point_t casualties_sum = fixed_point_t::_0; // the casualties_taken_mult of the regiments, to be averaged
	const std::span<const CompanyState> company_states = country.get_company_states();
	const auto company_buff = [&](const scoped_effect_t effect, std::span<const ScopeWeight> weights) -> fixed_point_t {
		return get_company_buff(company_types, company_states, company_counts, company_width, cache, effect, weights);
	};
	fixed_point_t fortification_sum = fixed_point_t::_0;
	std::array<memory::vector<ScopeWeight>, DIVISION_MAX_TYPES> scope_weights;
	std::array<bool, DIVISION_MAX_TYPES> scope_weights_made {};
	const auto scope_weights_of = [&](RegimentType const& regiment_type) -> memory::vector<ScopeWeight> const& {
		const std::size_t kind = get_division_slot_kind(regiment_type);
		if (!scope_weights_made[kind]) {
			scope_weights_made[kind] = true;
			scope_weights[kind] = get_scope_weights(cache, regiment_type);
		}
		return scope_weights[kind];
	};
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
			EquipmentDefinition const* const first = info.weapon.initial; // what the unit is built with
			const bool has_weapon = first != nullptr;
			// The weapon of the goods and the ammunition: the best allowed one, else what the unit is built with
			EquipmentDefinition const* const goods_weapon = info.weapon.best != nullptr ? info.weapon.best : first;
			EquipmentDefinition const* const effective = info.weapon.effective;
			const fixed_point_t keep = info.weapon.keep;
			const fixed_point_t attack_ratio = has_weapon
				? battle_formulas::weapon_ratio(first->attack_mult, effective->attack_mult * keep, equipment_fill) : fixed_point_t::_1;
			const fixed_point_t defence_ratio = has_weapon
				? battle_formulas::weapon_ratio(first->defense_mult, effective->defense_mult * keep, equipment_fill) : fixed_point_t::_1;
			const fixed_point_t breakthrough_ratio = has_weapon
				? battle_formulas::weapon_ratio(first->breakthrough_mult, effective->breakthrough_mult * keep, equipment_fill) : fixed_point_t::_1;
			const fixed_point_t row_weight = battle_formulas::row_weight(row, limits.row_weight_shift);

			stats.regiments++;
			cells_in_rank[rank]++;
			memory::vector<ScopeWeight> const& weights = scope_weights_of(*regiment_type);
			// The percents of the modifiers of the country and of the companies are summed (docs/WAR_ASSUMPTIONS.md, section 5)
			const fixed_point_t attack_factor = fixed_point_t::_1 + get_scoped_sum(country, cache, scoped_effect_t::ATTACK_MULT, weights)
				+ company_buff(scoped_effect_t::ATTACK_MULT, weights);
			const fixed_point_t defence_factor = fixed_point_t::_1 + get_scoped_sum(country, cache, scoped_effect_t::DEFENSE_MULT, weights)
				+ company_buff(scoped_effect_t::DEFENSE_MULT, weights);
			const fixed_point_t breakthrough_factor = fixed_point_t::_1 + get_scoped_sum(country, cache, scoped_effect_t::BREAKTHROUGH_MULT, weights)
				+ company_buff(scoped_effect_t::BREAKTHROUGH_MULT, weights);
			casualties_sum += get_scoped_product(country, cache, scoped_effect_t::CASUALTIES_TAKEN_MULT, weights);
			stats.attack += regiment_type->attack * attack_ratio * attack_factor * row_weight;
			stats.defence += regiment_type->defence * defence_ratio * defence_factor * row_weight;
			stats.breakthrough += regiment_type->get_breakthrough() * breakthrough_ratio * breakthrough_factor * row_weight;
			durability_sum += regiment_type->max_strength * defence_ratio;
			strength_sum += regiment_type->max_strength;
			organisation_sum += regiment_type->default_organisation;
			stats.reconnaissance += regiment_type->reconnaissance;
			speed = stats.regiments == 1 ? regiment_type->maximum_speed : std::min(speed, regiment_type->maximum_speed);
			stats.weight += regiment_type->ov.weight > fixed_point_t::_0 ? regiment_type->ov.weight
				: regiment_type->max_strength * defines.war.template_kit_weight_per_strength;
			// The fortification is the average of the regiments (a regiment of cavalry with no bonus has 0)
			fortification_sum += get_scoped_sum(country, cache, scoped_effect_t::FORTIFICATION, weights) + company_buff(scoped_effect_t::FORTIFICATION, weights);

			// The goods: the weapon of the line is the one of the best generation (the good of the first generation is the one of the base game)
			const std::string base_good = has_weapon
				? std::string { first->good.identifier }
				: std::string {};
			const auto good_id = [&](GoodDefinition const& good) -> std::string {
				if (has_weapon && good.get_identifier() == base_good) {
					return std::string { goods_weapon->good.identifier };
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
				stats.weight += pieces * goods_weapon->weight;
				ammo[goods_weapon->ammo_good.empty() ? std::string { "ammunition" } : std::string { goods_weapon->ammo_good }] +=
					pieces * goods_weapon->ammo_per_day;
			}
		}
	}

	for (const std::uint32_t cells : cells_in_rank) {
		stats.width += cells > 0;
	}
	// The companies are worth their cost, weight and men, and use their goods every day
	for (std::size_t index = 0; index < company_counts.size(); ++index) {
		if (company_counts[index] == 0) {
			continue;
		}
		CompanyType const& company = company_types.at(index);
		const std::int32_t count = static_cast<std::int32_t>(company_counts[index]);
		stats.companies += company_counts[index];
		stats.weight += company.weight * count;
		company_men += company.manpower * count;
		for (auto const& [good, amount] : company.cost) {
			cost[std::string { good->get_identifier() }] += amount * count;
		}
		for (auto const& [good, amount] : company.goods) {
			supply[std::string { good->get_identifier() }] += amount * count;
		}
	}
	{
		// The buff for a regiment of the scope all
		memory::vector<ScopeWeight> all_scope;
		if (const std::size_t all_index = cache.find_unit_scope("all"); all_index != ModifierEffectCache::NO_SCOPE) {
			all_scope.push_back({ all_index, fixed_point_t::_1 });
		}
		stats.company_attack = company_buff(scoped_effect_t::ATTACK_MULT, all_scope);
		stats.company_defence = company_buff(scoped_effect_t::DEFENSE_MULT, all_scope);
		stats.company_breakthrough = company_buff(scoped_effect_t::BREAKTHROUGH_MULT, all_scope);
		stats.company_fortification = company_buff(scoped_effect_t::FORTIFICATION, all_scope);
	}
	if (stats.regiments > 0) {
		stats.depth = fixed_point_t(static_cast<std::int32_t>(stats.regiments)) / static_cast<std::int32_t>(stats.width);
		stats.organisation = organisation_sum / static_cast<std::int32_t>(stats.regiments);
	}
	stats.speed = speed;
	stats.durability = durability_sum * (fixed_point_t::_1 + effect_value(cache.get_division_durability()));
	// The density: depth over width, changed by the technologies, its effect is on the attack, the defence, the breakthrough and the casualties
	stats.raw_density = battle_formulas::raw_density(defines.war, stats.regiments, stats.width);
	stats.density = battle_formulas::effective_density(
		stats.raw_density, stats.regiments > 0 ? get_effect_product(country, cache.get_density_coefficient_mult()) : fixed_point_t::_1,
		effect_value(cache.get_density_excess_reduction())
	);
	if (stats.regiments > 0) {
		const battle_formulas::DensityEffect density = battle_formulas::density_effect(defines.war, stats.density);
		stats.attack = stats.attack * density.attack_mult;
		stats.defence = stats.defence * density.defence_mult;
		stats.breakthrough = stats.breakthrough * density.breakthrough_mult;
		stats.casualties_mult = casualties_sum / static_cast<std::int32_t>(stats.regiments) * density.casualties_mult;
	}
	stats.fortification = fortification_sum / static_cast<std::int32_t>(std::max<std::uint32_t>(stats.regiments, 1));
	stats.reconnaissance += effect_value(cache.get_division_reconnaissance());
	stats.occupation_speed = defines.war.occupation_speed_base * (fixed_point_t::_1 + effect_value(cache.get_occupation_speed_pct()))
		* get_effect_product(country, cache.get_occupation_speed_mult());
	stats.occupation_instant = stats.occupation_speed * 100 >= fixed_point_t(defines.war.template_occupation_instant_percent);
	stats.men = (strength_sum + company_men) * defines.war.template_men_per_strength;

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

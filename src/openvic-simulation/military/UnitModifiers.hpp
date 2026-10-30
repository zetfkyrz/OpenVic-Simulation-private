#pragma once

#include <cstddef>
#include <span>
#include <string_view>

#include "openvic-simulation/core/memory/Vector.hpp"
#include "openvic-simulation/core/object/FixedPoint.hpp"
#include "openvic-simulation/types/UnitBranchType.hpp"

namespace OpenVic {
	struct CountryInstance;
	struct ModifierEffect;
	struct ModifierEffectCache;

	// The modifiers of the army with a scope (docs/WAR_ASSUMPTIONS.md, section 5). A modifier of a country can say to whom it applies: all, a category of
	// units (infantry, cavalry, artillery) or the id of a unit type; in a file of a technology, a unit or a company it is written
	//     attack_mult = { all = 0.05 infantry = 0.10 hussars = 0.02 }
	// (the existing form of the complex modifiers, like factory_goods_output = { small_arms = 0.1 }); a modifier without a scope (occupation_speed_pct = 0.25)
	// is for the whole country. How the values are put together (the rules of the project):
	//  - the effects with the suffix _pct and the plain bonuses (attack_mult, defense_mult, breakthrough_mult, organisation_regain, fortification,
	//    supply_days_add, ...) are SUMMED: 1 + the sum is the factor, the levels of the fortification and the days are added;
	//  - the effects that are real multipliers (casualties_taken_mult, occupation_speed_mult, mobilisation_speed_mult, density_coefficient_mult) are MULTIPLIED.
	// A unit type has weights of the scopes (all 1, its category 1, its id 1 by default; the dragoons have infantry 0.5 and cavalry 0.5): the value of a
	// scope counts that much for the unit.
	enum struct scoped_effect_t : std::size_t {
		ATTACK_MULT, DEFENSE_MULT, BREAKTHROUGH_MULT, FORTIFICATION, CASUALTIES_TAKEN_MULT, ORGANISATION_REGAIN, COUNT
	};
	inline constexpr std::size_t SCOPED_EFFECT_COUNT = static_cast<std::size_t>(scoped_effect_t::COUNT);

	struct ScopeWeight {
		std::size_t scope = 0; // the index in ModifierEffectCache::get_unit_scopes
		fixed_point_t weight = fixed_point_t::_1;
	};

	// The category scope of a regiment type: infantry, cavalry or artillery ("" for the others)
	[[nodiscard]] std::string_view get_unit_category_scope(RegimentType const& regiment_type);
	// The scopes a regiment type belongs to, with the weights
	[[nodiscard]] memory::vector<ScopeWeight> get_scope_weights(ModifierEffectCache const& cache, RegimentType const& regiment_type);

	// Σ weight × the value of the modifiers of the country for the scope
	[[nodiscard]] fixed_point_t get_scoped_sum(
		CountryInstance const& country, ModifierEffectCache const& cache, scoped_effect_t effect, std::span<const ScopeWeight> weights
	);
	// The product over the modifiers of 1 + weight × (value − 1): a modifier that halves the casualties of a unit that counts half for the scope
	// takes a quarter off
	[[nodiscard]] fixed_point_t get_scoped_product(
		CountryInstance const& country, ModifierEffectCache const& cache, scoped_effect_t effect, std::span<const ScopeWeight> weights
	);
	// The product of the values of the modifiers of a country that have the effect (1 if there are none)
	[[nodiscard]] fixed_point_t get_effect_product(CountryInstance const& country, ModifierEffect const* effect);
}

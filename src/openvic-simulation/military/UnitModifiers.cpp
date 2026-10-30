#include "UnitModifiers.hpp"

#include "openvic-simulation/country/CountryInstance.hpp"
#include "openvic-simulation/modifier/ModifierEffectCache.hpp"
#include "openvic-simulation/military/UnitType.hpp"

using namespace OpenVic;

std::string_view OpenVic::get_unit_category_scope(RegimentType const& regiment_type) {
	using enum UnitType::unit_category_t;
	switch (regiment_type.unit_category) {
	case INFANTRY: return "infantry";
	case CAVALRY: return "cavalry";
	default:
		return regiment_type.get_identifier().find("artillery") != std::string_view::npos ? "artillery" : std::string_view {};
	}
}

memory::vector<ScopeWeight> OpenVic::get_scope_weights(ModifierEffectCache const& cache, RegimentType const& regiment_type) {
	memory::vector<ScopeWeight> weights;
	for (std::string_view const scope : { std::string_view { "all" }, get_unit_category_scope(regiment_type), regiment_type.get_identifier() }) {
		if (scope.empty()) {
			continue;
		}
		const std::size_t index = cache.find_unit_scope(scope);
		if (index == ModifierEffectCache::NO_SCOPE) {
			continue;
		}
		const bool known = std::any_of(weights.begin(), weights.end(), [index](ScopeWeight const& weight) { return weight.scope == index; });
		if (!known) {
			weights.push_back({ index, fixed_point_t::_1 });
		}
	}
	return weights;
}

fixed_point_t OpenVic::get_scoped_sum(
	CountryInstance const& country, ModifierEffectCache const& cache, const scoped_effect_t effect, std::span<const ScopeWeight> weights
) {
	fixed_point_t sum = fixed_point_t::_0;
	for (ScopeWeight const& weight : weights) {
		ModifierEffect const* const scoped = cache.get_scoped_effect(static_cast<std::size_t>(effect), weight.scope);
		if (scoped != nullptr) {
			sum += country.get_modifier_effect_value(*scoped) * weight.weight;
		}
	}
	return sum;
}

fixed_point_t OpenVic::get_scoped_product(
	CountryInstance const& country, ModifierEffectCache const& cache, const scoped_effect_t effect, std::span<const ScopeWeight> weights
) {
	fixed_point_t product = fixed_point_t::_1;
	for (ScopeWeight const& weight : weights) {
		ModifierEffect const* const scoped = cache.get_scoped_effect(static_cast<std::size_t>(effect), weight.scope);
		if (scoped == nullptr) {
			continue;
		}
		const fixed_point_t scope_weight = weight.weight;
		country.for_each_contributing_modifier(*scoped, [&product, scope_weight](auto const&, const fixed_point_t value) {
			product = product * (fixed_point_t::_1 + scope_weight * (value - fixed_point_t::_1));
		});
	}
	return product;
}

fixed_point_t OpenVic::get_effect_product(CountryInstance const& country, ModifierEffect const* effect) {
	fixed_point_t product = fixed_point_t::_1;
	if (effect == nullptr) {
		return product;
	}
	country.for_each_contributing_modifier(*effect, [&product](auto const&, const fixed_point_t value) {
		product = product * value;
	});
	return product;
}

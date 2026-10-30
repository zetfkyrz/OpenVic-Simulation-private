#include "UnitModifiers.hpp"

#include "openvic-simulation/country/CountryInstance.hpp"
#include "openvic-simulation/modifier/ModifierEffectCache.hpp"
#include "openvic-simulation/military/UnitType.hpp"

using namespace OpenVic;

std::string_view OpenVic::get_unit_category_scope(RegimentType const& regiment_type) {
	if (regiment_type.ov.is_ov) {
		return regiment_type.ov.category_scope;
	}
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
	const auto add = [&](std::string_view scope, const fixed_point_t weight) {
		if (scope.empty()) {
			return;
		}
		const std::size_t index = cache.find_unit_scope(scope);
		if (index == ModifierEffectCache::NO_SCOPE) {
			return;
		}
		for (ScopeWeight& known : weights) {
			if (known.scope == index) {
				known.weight = weight;
				return;
			}
		}
		weights.push_back({ index, weight });
	};
	// All the units are in "all" and in their own id; the categories come from the file of the type (the dragoons: infantry 0.5, cavalry 0.5)
	add("all", fixed_point_t::_1);
	add(regiment_type.get_identifier(), fixed_point_t::_1);
	if (!regiment_type.ov.scope_weights.empty()) {
		for (auto const& [scope, weight] : regiment_type.ov.scope_weights) {
			add(scope, weight);
		}
	} else {
		add(get_unit_category_scope(regiment_type), fixed_point_t::_1);
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

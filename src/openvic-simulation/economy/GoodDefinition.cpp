#include "GoodDefinition.hpp"

#include "openvic-simulation/military/Equipment.hpp"

#include "openvic-simulation/modifier/ModifierManager.hpp"
#include "openvic-simulation/core/memory/FixedVector.hpp"
#include "openvic-simulation/core/string/Utility.hpp"

using namespace OpenVic;
using namespace OpenVic::NodeTools;

GoodCategory::GoodCategory(std::string_view new_identifier, index_t new_index)
	: HasIdentifier { new_identifier }, HasIndex { new_index } {}

GoodDefinition::GoodDefinition(
	std::string_view new_identifier,
	colour_t new_colour,
	index_t new_index,
	GoodCategory const& new_category,
	fixed_point_t new_base_price,
	bool new_is_available_from_start,
	bool new_is_tradeable,
	bool new_is_money,
	bool new_counters_overseas_penalty
) : HasIdentifierAndColour { new_identifier, new_colour, false },
	HasIndex { new_index },
	category { new_category },
	base_price { new_base_price },
	is_available_from_start { new_is_available_from_start },
	is_tradeable { new_is_tradeable },
	is_money { new_is_money },
	counters_overseas_penalty { new_counters_overseas_penalty } {}

bool GoodDefinitionManager::add_good_category(std::string_view identifier, size_t expected_goods_in_category) {
	if (identifier.empty()) {
		spdlog::error_s("Invalid good category identifier - empty!");
		return false;
	}

	if (good_categories.emplace_item(
		identifier,
		identifier, index_from_count<GoodCategory::index_t>(get_good_category_count())
	)) {
		good_categories.back().good_definitions.reserve(expected_goods_in_category);
		return true;
	} else {
		return false;
	}
}

bool GoodDefinitionManager::add_good_definition(
	std::string_view identifier, colour_t colour, GoodCategory& category, fixed_point_t base_price,
	bool is_available_from_start, bool is_tradeable, bool is_money, bool has_overseas_penalty
) {
	if (identifier.empty()) {
		spdlog::error_s("Invalid good identifier - empty!");
		return false;
	}
	if (base_price <= 0) {
		spdlog::error_s("Invalid base price for {}: {}", identifier, base_price);
		return false;
	}

	if (is_tradeable == is_money) {
		spdlog::warn_s(
			"Good {} has tradeable: {} and money: {}. Money goods are never tradeable. All other goods are tradeable. Setting tradeable has no effect.",
			identifier, bool_to_yes_no(is_tradeable), bool_to_yes_no(is_money)
		);
	}

	if (good_definitions.emplace_item(
		identifier,
		identifier, colour, index_from_count<GoodDefinition::index_t>(get_good_definition_count()), category, base_price, is_available_from_start,
		is_tradeable, is_money, has_overseas_penalty
	)) {
		category.good_definitions.emplace_back(get_back_good_definition());
		return true;
	} else {
		return false;
	}
}

bool GoodDefinitionManager::load_goods_file(ast::NodeCPtr root, EquipmentManager const* equipment_manager) {
	size_t total_expected_goods = 0;

	renamed_goods.clear();
	bool ret = expect_dictionary_reserve_length(
		good_categories,
		[this, &total_expected_goods](std::string_view key, ast::NodeCPtr value) -> bool {
			size_t expected_goods_in_category = 0;

			bool ret = expect_length(assign_variable_callback(expected_goods_in_category))(value);

			if (add_good_category(key, expected_goods_in_category)) {
				total_expected_goods += expected_goods_in_category;
			} else {
				ret = false;
			}

			return ret;
		}
	)(root);

	lock_good_categories();
	// The goods of the weapons that are new need room too (the categories keep references to the goods: the storage must not move)
	reserve_more_good_definitions(total_expected_goods + (equipment_manager != nullptr ? equipment_manager->get_equipment().size() : 0));

	ret &= good_categories.expect_item_dictionary(
		[this, equipment_manager](GoodCategory& good_category, ast::NodeCPtr good_category_value) -> bool {
			return expect_dictionary(
				[this, equipment_manager, &good_category](std::string_view key, ast::NodeCPtr value) -> bool {
					colour_t colour = colour_t::null();
					fixed_point_t base_price;
					bool is_available_from_start = true, is_tradeable = true;
					bool is_money = false, has_overseas_penalty = false;

					bool ret = expect_dictionary_keys(
						"color", ONE_EXACTLY, expect_colour(assign_variable_callback(colour)),
						"cost", ONE_EXACTLY, expect_fixed_point(assign_variable_callback(base_price)),
						"available_from_start", ZERO_OR_ONE, expect_bool(assign_variable_callback(is_available_from_start)),
						"tradeable", ZERO_OR_ONE, expect_bool(assign_variable_callback(is_tradeable)),
						"money", ZERO_OR_ONE, expect_bool(assign_variable_callback(is_money)),
						"overseas_penalty", ZERO_OR_ONE, expect_bool(assign_variable_callback(has_overseas_penalty))
					)(value);

					// A good that a weapon replaces is registered under the identifier and with the price of the weapon (the other values stay)
					std::string_view identifier = key;
					if (equipment_manager != nullptr) {
						for (EquipmentDefinition const& equipment : equipment_manager->get_equipment()) {
							if (!equipment.good.replaces.empty() && equipment.good.replaces == key) {
								identifier = equipment.good.identifier;
								base_price = equipment.good.base_price;
								renamed_goods.emplace_back(memory::string { key }, memory::string { identifier });
								break;
							}
						}
					}

					ret &= add_good_definition(
						identifier, colour, good_category, base_price, is_available_from_start, is_tradeable, is_money,
						has_overseas_penalty
					);

					return ret;
				}
			)(good_category_value);
		}
	)(root);

	// The old names find the renamed goods
	for (auto const& [old_name, new_name] : renamed_goods) {
		if (!good_definitions.add_identifier_alias(old_name, new_name)) {
			spdlog::warn_s("The good {} cannot be called {} too", new_name, old_name);
		}
	}
	// The goods of the other weapons, in the order the weapons are loaded
	if (equipment_manager != nullptr) {
		for (EquipmentDefinition const& equipment : equipment_manager->get_equipment()) {
			EquipmentGoodDescription const& description = equipment.good;
			if (good_definitions.has_identifier(description.identifier)) {
				continue; // the good of a weapon that replaced a good of the base game
			}
			GoodCategory* category = good_categories.get_item_by_identifier(description.category);
			if (category == nullptr) {
				spdlog::warn_s(
					"The good {} of the weapon {} is in the category {} that does not exist, it goes to the first category",
					description.identifier, equipment.identifier, description.category
				);
				category = &good_categories.front();
			}
			ret &= add_good_definition(
				description.identifier, description.colour, *category, description.base_price, description.available_from_start, true, false,
				false
			);
		}
	}

	lock_good_definitions();

	return ret;
}

bool GoodDefinitionManager::generate_modifiers(ModifierManager& modifier_manager) const {
	static constexpr bool HAS_NO_EFFECT = true;

	using enum ModifierEffect::format_t;
	using enum ModifierEffect::target_t;

	memory::FixedVector<ModifierEffectCache::good_effects_t, good_index_t>& good_effects = modifier_manager.modifier_effect_cache.good_effects;
	good_effects = std::move(
		decltype(ModifierEffectCache::good_effects) {
			generate_values,
			good_index_t(get_good_definition_count())
		}
	);

	bool ret = true;

	ret &= modifier_manager.register_complex_modifier("rgo_goods_input");
	ret &= modifier_manager.register_complex_modifier("rgo_goods_output");
	ret &= modifier_manager.register_complex_modifier("rgo_goods_throughput");
	ret &= modifier_manager.register_complex_modifier("factory_goods_input");
	ret &= modifier_manager.register_complex_modifier("factory_goods_output");
	ret &= modifier_manager.register_complex_modifier("factory_goods_throughput");
	ret &= modifier_manager.register_complex_modifier("artisan_goods_input");
	ret &= modifier_manager.register_complex_modifier("artisan_goods_output");
	ret &= modifier_manager.register_complex_modifier("artisan_goods_throughput");
	ret &= modifier_manager.register_complex_modifier("rgo_size");

	for (GoodDefinition const& good : get_good_definitions()) {
		const std::string_view good_identifier = good.get_identifier();
		ModifierEffectCache::good_effects_t& this_good_effects = good_effects[good.index];

		const auto good_modifier = [&modifier_manager, &ret, &good_identifier](
			ModifierEffect const*& effect_cache, std::string_view name, ModifierEffect::format_t format,
			std::string_view localisation_key, bool has_no_effect = false
		) -> void {
			ret &= modifier_manager.register_technology_modifier_effect(
				effect_cache, ModifierManager::get_flat_identifier(name, good_identifier),
				format, localisation_key, has_no_effect
			);
		};

		const auto make_production_localisation_suffix = [&good](
			std::string_view localisation_suffix
		) -> memory::string {
			return memory::fmt::format("${}$ ${}$", good, localisation_suffix);
		};

		good_modifier(
			this_good_effects.rgo_goods_input, "rgo_goods_input", FORMAT_x100_1DP_PC_NEG,
			make_production_localisation_suffix("TECH_INPUT"), HAS_NO_EFFECT
		);
		good_modifier(
			this_good_effects.rgo_goods_output, "rgo_goods_output", FORMAT_x100_1DP_PC_POS,
			make_production_localisation_suffix("TECH_OUTPUT")
		);
		good_modifier(
			this_good_effects.rgo_goods_throughput, "rgo_goods_throughput", FORMAT_x100_1DP_PC_POS,
			make_production_localisation_suffix("TECH_THROUGHPUT")
		);
		good_modifier(
			this_good_effects.factory_goods_input, "factory_goods_input", FORMAT_x100_1DP_PC_NEG,
			make_production_localisation_suffix("TECH_INPUT")
		);
		good_modifier(
			this_good_effects.factory_goods_output, "factory_goods_output", FORMAT_x100_1DP_PC_POS,
			make_production_localisation_suffix("TECH_OUTPUT")
		);
		good_modifier(
			this_good_effects.factory_goods_throughput, "factory_goods_throughput", FORMAT_x100_1DP_PC_POS,
			make_production_localisation_suffix("TECH_THROUGHPUT")
		);
		good_modifier(
			this_good_effects.artisan_goods_input, "artisan_goods_input", FORMAT_x100_1DP_PC_NEG,
			make_production_localisation_suffix("TECH_INPUT"), HAS_NO_EFFECT
		);
		good_modifier(
			this_good_effects.artisan_goods_output, "artisan_goods_output", FORMAT_x100_1DP_PC_POS,
			make_production_localisation_suffix("TECH_OUTPUT"), HAS_NO_EFFECT
		);
		good_modifier(
			this_good_effects.artisan_goods_throughput, "artisan_goods_throughput", FORMAT_x100_1DP_PC_POS,
			make_production_localisation_suffix("TECH_THROUGHPUT"), HAS_NO_EFFECT
		);
		good_modifier(
			this_good_effects.rgo_size, "rgo_size", FORMAT_x100_2DP_PC_POS,
			memory::fmt::format("{}_RGO_SIZE", good)
		);
	}

	// The files of Vic2 write the modifiers of a good that a weapon renamed under its old name
	for (auto const& [old_name, new_name] : renamed_goods) {
		for (std::string_view const name : {
			"rgo_goods_input", "rgo_goods_output", "rgo_goods_throughput", "factory_goods_input", "factory_goods_output",
			"factory_goods_throughput", "artisan_goods_input", "artisan_goods_output", "artisan_goods_throughput", "rgo_size"
		}) {
			ret &= modifier_manager.add_technology_modifier_alias(
				ModifierManager::get_flat_identifier(name, old_name), ModifierManager::get_flat_identifier(name, new_name)
			);
		}
	}

	return ret;
}

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <utility>

#include "openvic-simulation/core/memory/String.hpp"
#include "openvic-simulation/core/memory/Vector.hpp"
#include "openvic-simulation/core/object/FixedPoint.hpp"
#include "openvic-simulation/dataloader/NodeTools.hpp"
#include "openvic-simulation/modifier/Modifier.hpp"

namespace OpenVic {
	struct BuildingType;
	struct CountryInstance;
	struct EquipmentDefinition;
	struct ProductionType;
	struct EquipmentManager;
	struct InstanceManager;
	struct ModifierManager;

	// The unlocks of a country (docs/EQUIPMENT_MODDING.md, section 6). The layer does not depend on the system of technologies: whatever opens
	// a weapon, a formation or a company of support gives the country a key, and a modifier for the numbers (the limits of the templates).
	//
	//  - keys: "<kind>:<id>", for example equipment:rifle_2, formation:<id>, support_company:<id>.
	//    CountryInstance::grant_unlock / revoke_unlock / has_unlock, the keys are in the state hash.
	//  - numbers: the effects max_columns, max_rows, max_company_slots, density_reduction and row_weight_shift are ordinary modifier effects
	//    (a technology, an event modifier, a unlock modifier can give them); the modifier sum of the country adds them up by the rules of the project.
	//    A modifier that an unlock gives: CountryInstance::add_unlock_modifier.

	// "<kind>:<id>": both parts are not empty and have no spaces
	[[nodiscard]] bool is_valid_unlock_key(std::string_view key);

	// The keys of the mechanics that the technologies of the army open (kind "mechanic", docs/WAR_ASSUMPTIONS.md): a country without the key cannot do it
	inline constexpr std::string_view UNLOCK_MECHANIC_DIVISION_TEMPLATES = "mechanic:division_templates"; // create, change and delete templates (the standard one is fixed)
	inline constexpr std::string_view UNLOCK_MECHANIC_MOBILISATION = "mechanic:mobilisation"; // mobilise (demobilising is always possible)
	inline constexpr std::string_view UNLOCK_MECHANIC_INSTANT_OCCUPATION = "mechanic:instant_occupation"; // the divisions take a province at once

	// A modifier of the provisional unlocks (the type is a technology: the effects are the ones of the technologies)
	struct UnlockModifier : Modifier {
		UnlockModifier(std::string_view new_identifier, ModifierValue&& new_values)
			: Modifier { new_identifier, std::move(new_values), modifier_type_t::TECHNOLOGY } {}
		UnlockModifier(UnlockModifier&&) = default;
	};

	struct ProvisionalUnlockStep {
		int year = 0;
		memory::vector<memory::string> keys;
		std::optional<UnlockModifier> modifier;
	};

	// TEMPORARY: ov_data/common/ov_equipment/provisional_unlocks.txt gives the keys and the modifiers by the year of the game until the new
	// system of technologies does it (docs/PENDING_PLANS.md). ov_war.PROVISIONAL_UNLOCKS_ENABLED = 0 turns it off. The steps are sorted by the year
	// and do not change after loading (the countries point to their modifiers).
	struct ProvisionalUnlocks {
	private:
		memory::vector<ProvisionalUnlockStep> steps;

	public:
		// A broken step is skipped with a warning. Returns the number of steps loaded.
		std::size_t load_file(std::string_view source, ast::NodeCPtr root, ModifierManager const& modifier_manager);

		[[nodiscard]] std::span<const ProvisionalUnlockStep> get_steps() const {
			return steps;
		}
	};

	// The keys that every country has from the start of the game (ov_data/common/ov_equipment/default_unlocks.txt: unlock = { "<key>" ... }), for example
	// the weapons of the Napoleonic wars. They are given again every day: a key that a country has lost is not given back by anything but this list.
	struct DefaultUnlocks {
	private:
		memory::vector<memory::string> keys;

	public:
		// Returns the number of keys. A key that is not <kind>:<id> is skipped with a warning.
		std::size_t load_file(std::string_view source, ast::NodeCPtr root);
		[[nodiscard]] std::span<const memory::string> get_keys() const {
			return keys;
		}
	};
	void apply_default_unlocks(InstanceManager& instance_manager);

	// Every day (one thread, before the modifier sums): the steps whose year has come give their keys and modifiers to every country
	void update_provisional_unlocks(InstanceManager& instance_manager);
	// The same for a given year (the tests)
	void apply_provisional_unlocks(InstanceManager& instance_manager, int year);

	// What a country may build into a template: the default of ov_war.TEMPLATE_* plus the effects of its modifiers, between 1 (0 for the
	// companies) and the maximum of ov_war
	struct DivisionTemplateLimits {
		std::uint32_t ranks = 0; // columns of the grid
		std::uint32_t depth = 0; // rows of the grid
		std::uint32_t company_slots = 0; // support company slots of a rank
		std::int32_t row_weight_shift = 0; // 0: the weights of the rows are 1, 1/2, 1/4, 1/8; 1: 1, 1, 1/2, 1/4
		fixed_point_t density_reduction = fixed_point_t::_0; // 0..1
	};
	[[nodiscard]] DivisionTemplateLimits get_template_limits(InstanceManager const& instance_manager, CountryInstance const& country);

	// A weapon of the country: it has the unlock key of the weapon
	[[nodiscard]] bool is_equipment_unlocked(CountryInstance const& country, EquipmentDefinition const& equipment);
	// The weapon of the line with the highest generation that the country has, nullptr if it has none
	[[nodiscard]] EquipmentDefinition const* get_best_equipment(
		EquipmentManager const& manager, CountryInstance const& country, std::string_view line
	);

	// What a factory of a building can make: the production type of the building and the types of ov_data/common/ov_production_types that name the
	// building (a type for every generation of a weapon), the lowest generation first. A type is available to a country when it has the unlock of the
	// weapon the type makes (the type of a good that is no weapon is always available).
	struct FactoryProductionOption {
		ProductionType const* production_type = nullptr;
		memory::string unlock_key; // empty: always available
		std::uint32_t tier = 0; // the generation of the weapon, 0 for a good that is no weapon
	};
	[[nodiscard]] memory::vector<FactoryProductionOption> get_factory_production_options(
		InstanceManager const& instance_manager, BuildingType const& building_type
	);
	[[nodiscard]] bool is_production_option_available(CountryInstance const& country, FactoryProductionOption const& option);
	// The type of the highest generation the country has (the type of the building if it has none of them)
	[[nodiscard]] ProductionType const* get_best_production_type(
		InstanceManager const& instance_manager, CountryInstance const& country, BuildingType const& building_type
	);
	// TODO: a simple choice for the AI: every factory of a weapon makes the newest generation the country has. The AI should choose by the demand, the
	// price and the margin of the types (docs/PENDING_PLANS.md).
	void choose_ai_production_types(InstanceManager& instance_manager, CountryInstance& country);
}

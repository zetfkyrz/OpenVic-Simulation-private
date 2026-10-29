#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string_view>
#include <utility>

#include "openvic-simulation/core/memory/String.hpp"
#include "openvic-simulation/core/memory/Vector.hpp"
#include "openvic-simulation/core/object/Colour.hpp"
#include "openvic-simulation/core/object/FixedPoint.hpp"
#include "openvic-simulation/dataloader/NodeTools.hpp"
#include "openvic-simulation/types/OrderedContainers.hpp"

namespace OpenVic {
	// The weapons of the army (docs/EQUIPMENT_MODDING.md). Nothing is hardcoded: every weapon is a file of ov_data/common/ov_equipment/<folder>/<id>.txt
	// (the folder is only for order), the kinds of weapons ("lines": rifle, field_artillery, later siege artillery, machine guns...) are described in
	// ov_data/common/ov_equipment/lines.txt. A mod adds a weapon or a line with files only, and replaces a built-in one with a file of the same path
	// (or with a weapon of the same id in another file).

	// A kind of weapons that regiments use: the generations of it are the weapons of this line
	struct EquipmentLine {
		memory::string identifier;
		memory::string icon; // the key of the icon (a sprite of the interface files)
		memory::string name_key; // localisation key
		memory::string slot; // the slot of the division template the line belongs to: infantry, cavalry, support, special
		memory::vector<memory::string> unit_types; // the identifiers of the regiment types that use the line
		memory::string source; // the file it was loaded from
	};

	// The good that a weapon is: registered from the file of the weapon with the goods of the game (step 4 of the plan)
	struct EquipmentGoodDescription {
		memory::string identifier;
		memory::string replaces; // the good of the base game that this one becomes (small_arms -> rifle_1), or empty
		fixed_point_t base_price = fixed_point_t::_0;
		memory::string icon;
		memory::string name_key;
		colour_t colour = colour_t::null();
		memory::string category = "military_goods"; // the category of goods the good is in
		bool available_from_start = true; // not for a good that replaces one of the base game: it keeps the setting of that one
	};

	struct EquipmentDefinition {
		memory::string identifier;
		std::size_t index = 0; // the position in the load order (a replaced weapon keeps the position)
		memory::string line; // EquipmentLine::identifier
		std::uint32_t tier = 1; // the generation, 1 and up
		memory::string unlock_key; // the unlock of the country that makes the weapon available (equipment:<id> if the file has none)

		// The strength of a piece in the points of the regiment types (units/*.txt: attack, defence). A regiment keeps the attack and the defence of
		// its type with the weapon of the first generation of its line and gets more or less with another one, in proportion to these numbers.
		fixed_point_t attack = fixed_point_t::_0;
		fixed_point_t defence = fixed_point_t::_0;
		fixed_point_t ammo_per_day = fixed_point_t::_0; // of ammo_good a piece uses in a day of battle
		memory::string ammo_good; // empty: the weapon does not use a special ammunition (the field is for the ammunition generations later)
		fixed_point_t weight = fixed_point_t::_0; // kilograms of a piece (carried by the ships)
		memory::vector<std::pair<memory::string, fixed_point_t>> cost; // goods (by identifier) a piece of it costs in a template
		memory::vector<memory::string> flags; // breech_loading, magazine, smokeless, indirect_fire, canister, shrapnel; sorted

		EquipmentGoodDescription good;
		memory::string source; // the file it was loaded from

		[[nodiscard]] bool has_flag(std::string_view flag) const;
	};

	struct EquipmentManager {
	private:
		memory::vector<EquipmentLine> lines;
		memory::vector<EquipmentDefinition> equipment;
		string_map_t<std::size_t> line_index_by_identifier;
		string_map_t<std::size_t> equipment_index_by_identifier;

	public:
		// lines.txt: a broken line is skipped with a warning (the file, the key, the reason), the rest is loaded. Returns the number of lines loaded.
		std::size_t load_lines_file(std::string_view source, ast::NodeCPtr root);
		// A file of weapons (one weapon, or several): the same, a weapon of an unknown line or with wrong values is skipped with a warning; a weapon
		// with the id of one that is loaded replaces it. Returns the number of weapons loaded.
		std::size_t load_equipment_file(std::string_view source, ast::NodeCPtr root);

		[[nodiscard]] std::span<const EquipmentLine> get_lines() const {
			return lines;
		}
		[[nodiscard]] std::span<const EquipmentDefinition> get_equipment() const {
			return equipment;
		}
		[[nodiscard]] EquipmentLine const* find_line(std::string_view identifier) const;
		// The line of weapons a regiment type uses (the line that names it in unit_types), nullptr if it has none
		[[nodiscard]] EquipmentLine const* find_line_of_unit_type(std::string_view unit_type_identifier) const;
		[[nodiscard]] EquipmentDefinition const* find_equipment(std::string_view identifier) const;
		// The weapons of a line, the lowest generation first
		[[nodiscard]] memory::vector<EquipmentDefinition const*> get_line_equipment(std::string_view line_identifier) const;
	};
}

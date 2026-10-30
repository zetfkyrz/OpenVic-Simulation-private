#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <span>
#include <string_view>

#include "openvic-simulation/core/memory/String.hpp"
#include "openvic-simulation/core/memory/Vector.hpp"
#include "openvic-simulation/dataloader/NodeTools.hpp"
#include "openvic-simulation/military/Unlocks.hpp"
#include "openvic-simulation/types/OrderedContainers.hpp"

namespace OpenVic {
	struct ModifierManager;

	// The technologies of the army of OpenVic (docs/WAR_ASSUMPTIONS.md, section 5): a tree on a grid, one technology a file in
	// ov_data/common/ov_technology/<branch>/<id>.txt. They are independent of the technologies of Vic2 (which are not touched). A technology gives keys of the
	// unlock layer (unlocks) and modifiers of the country (modifiers) when it is researched, nothing else.
	//
	//   arms_making = {
	//       era = 1  branch = army  x = 3  y = 0  research_days = 730  year_hint = 1836  start = civilised
	//       prerequisite = { a b }   # a block: ANY of the technologies in it (drawn dashed if there are two or more)
	//       prerequisite = { c }     # another block: a must (drawn solid). The blocks are joined by AND, the technologies of a block by OR.
	//       unlocks = { "building:small_arms_factory" }   modifiers = { attack_mult = { all = 0.05 } }
	//   }
	struct ArmyTechnology {
		memory::string identifier;
		std::size_t index = 0; // the position in the load order (the order of the files)
		memory::string branch;
		std::size_t branch_index = 0; // the position of the branch in the order the branches were met
		std::int32_t era = 1;
		std::int32_t x = 0, y = 0; // the cell of the grid
		std::int32_t research_days = 730;
		std::int32_t year_hint = 0; // a hint for the AI and the provisional layer, nothing more
		memory::string start; // "civilised": the civilised countries have it researched from the start of the game
		memory::vector<memory::vector<memory::string>> prerequisites; // blocks (AND) of the ids (OR)
		memory::vector<memory::string> unlocks; // keys of the unlock layer
		std::optional<UnlockModifier> modifier; // the modifiers of the country (nothing if the file has none)
		memory::string source; // the file it was loaded from
		std::int32_t icon_frame = 0; // reserved for the icon of the technology (the branch has one for now)
	};

	struct ArmyTechnologyManager {
	private:
		// A deque: the countries point to the modifiers of the technologies, which must not move
		std::deque<ArmyTechnology> technologies;
		string_map_t<std::size_t> index_by_identifier;
		memory::vector<memory::string> branches;

	public:
		// A broken technology is skipped with a warning (file, line, reason). Returns the number of technologies loaded.
		std::size_t load_file(std::string_view source, ast::NodeCPtr root, ModifierManager const& modifier_manager);
		// After all the files: the warnings about a missing prerequisite, a cycle, two technologies in a cell, a prerequisite that is not above. Returns
		// the number of problems.
		std::size_t validate() const;

		[[nodiscard]] std::size_t size() const {
			return technologies.size();
		}
		[[nodiscard]] ArmyTechnology const& at(const std::size_t index) const {
			return technologies[index];
		}
		[[nodiscard]] ArmyTechnology const* find(std::string_view identifier) const;
		[[nodiscard]] std::deque<ArmyTechnology> const& get_technologies() const {
			return technologies;
		}
		// The branches in the order they were met (a branch has one slot of research for a country)
		[[nodiscard]] memory::vector<memory::string> const& get_branches() const {
			return branches;
		}
		// The prerequisites with the indices: blocks of indices (a missing id gives no index, so the block cannot be satisfied by it)
		[[nodiscard]] memory::vector<memory::vector<std::size_t>> get_prerequisite_indices(ArmyTechnology const& technology) const;
	};

	// The states of a technology for a country
	enum struct tech_state_t : std::uint8_t { NOT_AVAILABLE, AVAILABLE, RESEARCHING, RESEARCHED };
}

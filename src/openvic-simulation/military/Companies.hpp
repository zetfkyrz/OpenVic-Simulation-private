#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <span>
#include <string_view>
#include <utility>

#include "openvic-simulation/core/memory/String.hpp"
#include "openvic-simulation/core/memory/Vector.hpp"
#include "openvic-simulation/core/object/FixedPoint.hpp"
#include "openvic-simulation/dataloader/NodeTools.hpp"
#include "openvic-simulation/military/Division.hpp"
#include "openvic-simulation/military/UnitModifiers.hpp"
#include "openvic-simulation/types/OrderedContainers.hpp"

namespace OpenVic {
	struct CountryInstance;
	struct GoodDefinition;
	struct GoodDefinitionManager;
	struct InstanceManager;
	struct ModifierEffectCache;

	// The support companies of the divisions (docs/WAR_ASSUMPTIONS.md, section 5): ov_data/common/ov_companies/<id>.txt, one company a file. A company
	// stands in a slot of a rank (a column) of a division template, uses goods of the national stockpile every day and buffs the division:
	//
	//   telegraph_company = {
	//       category = signals                    # a column holds one company of a category; a new category is a new word here
	//       unlock_key = "support_company:telegraph"   # the key of the unlock layer (none: open from the start)
	//       fallback = telegraph_company          # optional: what the company works as when its goods are not in the stockpile at all
	//       goods = { telegraph = 0.1 }           # the goods of a day for one company
	//       cost = { machine_parts = 5 }          # what a company is worth in a division (goods; only shown in the cost of the template)
	//       weight = 300  manpower = 0.2          # kilograms; strength points (a point is ov_war.TEMPLATE_MEN_PER_STRENGTH men)
	//       buffs = { attack_mult = { all = 0.05 } defense_mult = { all = 0.05 } breakthrough_mult = { all = 0.05 } fortification = { all = 1 } }
	//   }
	//
	// The buff of a category is Σ (the companies of the type in the division / the ranks of the division, at most 1) × the buff of the type, each
	// scaled by the share of its goods that the stockpile gives. The depth of the division does not matter.
	struct CompanyBuff {
		scoped_effect_t effect = scoped_effect_t::ATTACK_MULT; // attack_mult, defense_mult, breakthrough_mult or fortification
		memory::string scope; // all, infantry, cavalry, artillery or the id of a unit type
		fixed_point_t value;
	};

	struct CompanyType {
		memory::string identifier;
		std::size_t index = 0;
		memory::string category;
		memory::string unlock_key; // "" : open from the start
		memory::string fallback; // the identifier of the type it works as without its goods ("" : none)
		std::size_t fallback_index = SIZE_MAX;
		memory::vector<std::pair<GoodDefinition const*, fixed_point_t>> goods; // a day, one company
		memory::vector<std::pair<GoodDefinition const*, fixed_point_t>> cost;
		fixed_point_t weight;
		fixed_point_t manpower;
		memory::vector<CompanyBuff> buffs;
		memory::string source;
	};

	struct CompanyTypeManager {
	private:
		std::deque<CompanyType> company_types;
		string_map_t<std::size_t> index_by_identifier;
		memory::vector<memory::string> categories;

	public:
		// A broken company is skipped with a warning (file, line, reason). Returns the number loaded.
		std::size_t load_file(std::string_view source, ast::NodeCPtr root, GoodDefinitionManager const& good_definition_manager);
		// After all the files: the fallbacks are looked up (a fallback that does not exist is dropped with a warning)
		void finish_loading();

		[[nodiscard]] std::size_t size() const {
			return company_types.size();
		}
		[[nodiscard]] CompanyType const& at(const std::size_t index) const {
			return company_types[index];
		}
		[[nodiscard]] CompanyType const* find(std::string_view identifier) const;
		// The type for the number in a slot of a template (the index + 1, 0 is an empty slot)
		[[nodiscard]] CompanyType const* get_by_slot_value(const std::uint8_t slot_value) const {
			return slot_value == 0 || slot_value > company_types.size() ? nullptr : &company_types[slot_value - 1];
		}
		[[nodiscard]] std::deque<CompanyType> const& get_company_types() const {
			return company_types;
		}
		[[nodiscard]] memory::vector<memory::string> const& get_categories() const {
			return categories;
		}
	};

	// The ranks of a template that have a regiment (the width) and the number of companies of every type in them
	[[nodiscard]] std::uint32_t get_template_width(DivisionTemplate const& division_template);
	[[nodiscard]] memory::vector<std::uint32_t> count_template_companies(CompanyTypeManager const& manager, DivisionTemplate const& division_template);

	// A layout of companies a country may use: a slot below ov_war company slots of its limits, in a rank that has a regiment, a type that exists and the
	// country has the key of, a category once in a rank
	[[nodiscard]] bool is_valid_company_layout(
		InstanceManager const& instance_manager, CountryInstance const& country, DivisionGrid const& cells, DivisionCompanies const& companies
	);
	// The command: the companies of one template of the country (the cells stay). false if nothing changed or the layout is not allowed.
	bool set_division_template_companies(
		InstanceManager& instance_manager, CountryInstance& country, std::uint32_t template_id, DivisionCompanies const& companies
	);

	// The state of the companies of a country in a day (CountryInstance::get_company_state): which company a type works as and how much of its goods
	// the stockpile gave; (re)made every day while the goods are worked out
	struct CompanyState {
		std::size_t works_as = 0; // the type that works (the fallback while the goods of the type are not in the stockpile at all)
		fixed_point_t supplied = fixed_point_t::_1; // 0..1
	};

	// The buff of the companies of a template for a regiment type: Σ effect × weights, the shares of the types and their supplies in
	[[nodiscard]] fixed_point_t get_company_buff(
		CompanyTypeManager const& manager, std::span<const CompanyState> states, std::span<const std::uint32_t> counts, std::uint32_t width,
		ModifierEffectCache const& cache, scoped_effect_t effect, std::span<const ScopeWeight> weights
	);
}

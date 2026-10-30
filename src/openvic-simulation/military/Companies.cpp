#include "Companies.hpp"

#include <algorithm>
#include <fstream>
#include <string>

#include "openvic-simulation/DefinitionManager.hpp"
#include "openvic-simulation/InstanceManager.hpp"
#include "openvic-simulation/country/CountryInstance.hpp"
#include "openvic-simulation/economy/GoodDefinition.hpp"
#include "openvic-simulation/military/Unlocks.hpp"
#include "openvic-simulation/modifier/ModifierEffectCache.hpp"
#include "openvic-simulation/utility/Logger.hpp"

using namespace OpenVic;
using namespace OpenVic::NodeTools;

namespace {
	std::size_t line_of_key(std::string_view source, std::string_view key) {
		std::ifstream file { std::string { source } };
		std::string text;
		std::size_t line_number = 0;
		while (std::getline(file, text)) {
			line_number++;
			if (!key.empty() && text.find(key) != std::string::npos) {
				return line_number;
			}
		}
		return 0;
	}

	// attack_mult = { all = 0.05 infantry = 0.1 } or attack_mult = 0.05 (all)
	node_callback_t expect_company_buff(memory::vector<CompanyBuff>& buffs, const scoped_effect_t effect) {
		return [&buffs, effect](ast::NodeCPtr node) -> bool {
			if (dryad::node_try_cast<ast::ListValue>(node) != nullptr) {
				return expect_dictionary([&buffs, effect](std::string_view scope, ast::NodeCPtr value) -> bool {
					fixed_point_t amount = fixed_point_t::_0;
					const bool ok = expect_fixed_point(assign_variable_callback(amount))(value);
					buffs.push_back({ effect, memory::string { scope }, amount });
					return ok;
				})(node);
			}
			fixed_point_t amount = fixed_point_t::_0;
			const bool ok = expect_fixed_point(assign_variable_callback(amount))(node);
			buffs.push_back({ effect, "all", amount });
			return ok;
		};
	}
}

std::size_t CompanyTypeManager::load_file(std::string_view source, ast::NodeCPtr root, GoodDefinitionManager const& good_definition_manager) {
	std::size_t loaded = 0;

	const bool parsed = expect_dictionary([&](std::string_view key, ast::NodeCPtr value) -> bool {
		CompanyType company;
		company.identifier = key;
		company.source = source;
		const auto skip = [&](std::string_view reason) -> bool {
			const std::size_t line = line_of_key(source, key);
			spdlog::warn_s("Company \"{}\" skipped: {} ({}:{})", key, reason, source, line);
			return true;
		};

		const bool ok = expect_dictionary_keys_and_default(
			[source, key](std::string_view unknown_key, ast::NodeCPtr) -> bool {
				spdlog::warn_s("Unknown key \"{}\" in the company \"{}\" ({})", unknown_key, key, source);
				return true;
			},
			"category", ONE_EXACTLY, expect_identifier_or_string(assign_variable_callback_string(company.category)),
			"unlock_key", ZERO_OR_ONE, expect_identifier_or_string(assign_variable_callback_string(company.unlock_key)),
			"fallback", ZERO_OR_ONE, expect_identifier_or_string(assign_variable_callback_string(company.fallback)),
			"goods", ZERO_OR_ONE, good_definition_manager.expect_good_definition_decimal_map(
				[&company](fixed_point_map_t<GoodDefinition const*>&& goods) -> bool {
					for (auto const& [good, amount] : goods) {
						company.goods.emplace_back(good, amount);
					}
					return true;
				}
			),
			"cost", ZERO_OR_ONE, good_definition_manager.expect_good_definition_decimal_map(
				[&company](fixed_point_map_t<GoodDefinition const*>&& goods) -> bool {
					for (auto const& [good, amount] : goods) {
						company.cost.emplace_back(good, amount);
					}
					return true;
				}
			),
			"weight", ZERO_OR_ONE, expect_fixed_point(assign_variable_callback(company.weight)),
			"manpower", ZERO_OR_ONE, expect_fixed_point(assign_variable_callback(company.manpower)),
			"buffs", ZERO_OR_ONE, expect_dictionary_keys_and_default(
				[source, key](std::string_view unknown_key, ast::NodeCPtr) -> bool {
					spdlog::warn_s("Unknown buff \"{}\" in the company \"{}\" ({})", unknown_key, key, source);
					return true;
				},
				"attack_mult", ZERO_OR_ONE, expect_company_buff(company.buffs, scoped_effect_t::ATTACK_MULT),
				"defense_mult", ZERO_OR_ONE, expect_company_buff(company.buffs, scoped_effect_t::DEFENSE_MULT),
				"breakthrough_mult", ZERO_OR_ONE, expect_company_buff(company.buffs, scoped_effect_t::BREAKTHROUGH_MULT),
				"fortification", ZERO_OR_ONE, expect_company_buff(company.buffs, scoped_effect_t::FORTIFICATION)
			)
		)(value);

		if (!ok) {
			return skip("the file could not be read as a company, see the messages above");
		}
		if (company.category.empty()) {
			return skip("the category is empty");
		}
		if (company_types.size() >= 255) {
			return skip("there are 255 companies already (a slot of a template holds a number up to 255)");
		}
		if (!company.unlock_key.empty() && !is_valid_unlock_key(company.unlock_key)) {
			return skip(memory::fmt::format("the unlock key \"{}\" is not <kind>:<id>", company.unlock_key));
		}
		if (index_by_identifier.contains(company.identifier)) {
			return skip("a company with this id was loaded already");
		}
		company.index = company_types.size();
		index_by_identifier.emplace(company.identifier, company.index);
		if (std::find(categories.begin(), categories.end(), company.category) == categories.end()) {
			categories.push_back(company.category);
		}
		company_types.push_back(std::move(company));
		loaded++;
		return true;
	})(root);

	if (!parsed) {
		spdlog::warn_s("The companies file {} is broken, what could be read is loaded", source);
	}
	return loaded;
}

void CompanyTypeManager::finish_loading() {
	for (CompanyType& company : company_types) {
		company.fallback_index = SIZE_MAX;
		if (company.fallback.empty()) {
			continue;
		}
		CompanyType const* const target = find(company.fallback);
		if (target == nullptr || target == &company) {
			spdlog::warn_s("The fallback \"{}\" of the company {} does not exist, dropped ({})", company.fallback, company.identifier, company.source);
			continue;
		}
		company.fallback_index = target->index;
	}
}

CompanyType const* CompanyTypeManager::find(std::string_view identifier) const {
	const auto it = index_by_identifier.find(identifier);
	return it == index_by_identifier.end() ? nullptr : &company_types[it->second];
}

std::uint32_t OpenVic::get_template_width(DivisionTemplate const& division_template) {
	std::uint32_t width = 0;
	for (std::size_t rank = 0; rank < DIVISION_MAX_RANKS; ++rank) {
		for (std::size_t row = 0; row < DIVISION_MAX_DEPTH; ++row) {
			if (division_template.cells[DivisionTemplate::cell_index(rank, row)] != 0) {
				width++;
				break;
			}
		}
	}
	return width;
}

memory::vector<std::uint32_t> OpenVic::count_template_companies(CompanyTypeManager const& manager, DivisionTemplate const& division_template) {
	memory::vector<std::uint32_t> counts(manager.size(), 0);
	for (std::size_t rank = 0; rank < DIVISION_MAX_RANKS; ++rank) {
		bool has_regiment = false;
		for (std::size_t row = 0; row < DIVISION_MAX_DEPTH; ++row) {
			has_regiment |= division_template.cells[DivisionTemplate::cell_index(rank, row)] != 0;
		}
		if (!has_regiment) {
			continue;
		}
		for (std::size_t slot = 0; slot < DIVISION_MAX_COMPANY_SLOTS; ++slot) {
			if (CompanyType const* const company = manager.get_by_slot_value(division_template.companies[rank * DIVISION_MAX_COMPANY_SLOTS + slot])) {
				counts[company->index]++;
			}
		}
	}
	return counts;
}

bool OpenVic::is_valid_company_layout(
	InstanceManager const& instance_manager, CountryInstance const& country, DivisionGrid const& cells, DivisionCompanies const& companies
) {
	const DivisionTemplateLimits limits = get_template_limits(instance_manager, country);
	CompanyTypeManager const& manager = instance_manager.definition_manager.get_military_manager().get_company_type_manager();
	for (std::size_t rank = 0; rank < DIVISION_MAX_RANKS; ++rank) {
		bool has_regiment = false;
		for (std::size_t row = 0; row < DIVISION_MAX_DEPTH; ++row) {
			has_regiment |= cells[DivisionTemplate::cell_index(rank, row)] != 0;
		}
		memory::vector<std::string_view> categories_in_rank;
		for (std::size_t slot = 0; slot < DIVISION_MAX_COMPANY_SLOTS; ++slot) {
			const std::uint8_t value = companies[rank * DIVISION_MAX_COMPANY_SLOTS + slot];
			if (value == 0) {
				continue;
			}
			CompanyType const* const company = manager.get_by_slot_value(value);
			if (company == nullptr || slot >= limits.company_slots || !has_regiment || rank >= limits.ranks) {
				return false;
			}
			if (!company->unlock_key.empty() && !country.has_unlock(company->unlock_key)) {
				return false;
			}
			// A rank holds one company of a category
			if (std::find(categories_in_rank.begin(), categories_in_rank.end(), std::string_view { company->category }) != categories_in_rank.end()) {
				return false;
			}
			categories_in_rank.push_back(company->category);
		}
	}
	return true;
}

bool OpenVic::set_division_template_companies(
	InstanceManager& instance_manager, CountryInstance& country, const std::uint32_t template_id, DivisionCompanies const& companies
) {
	if (!country.has_army_organisation()) {
		return false;
	}
	DivisionTemplate* division_template = nullptr;
	for (DivisionTemplate& candidate : country.get_mutable_division_templates()) {
		if (candidate.id == template_id) {
			division_template = &candidate;
		}
	}
	if (division_template == nullptr || division_template->companies == companies
		|| !is_valid_company_layout(instance_manager, country, division_template->cells, companies)) {
		return false;
	}
	division_template->companies = companies;
	return true;
}

fixed_point_t OpenVic::get_company_buff(
	CompanyTypeManager const& manager, std::span<const CompanyState> states, std::span<const std::uint32_t> counts, const std::uint32_t width,
	ModifierEffectCache const& cache, const scoped_effect_t effect, std::span<const ScopeWeight> weights
) {
	fixed_point_t sum = fixed_point_t::_0;
	if (width == 0) {
		return sum;
	}
	for (std::size_t index = 0; index < counts.size() && index < manager.size(); ++index) {
		if (counts[index] == 0) {
			continue;
		}
		const CompanyState state = index < states.size() ? states[index] : CompanyState { index, fixed_point_t::_1 };
		const fixed_point_t share = std::min(fixed_point_t::_1, fixed_point_t(static_cast<std::int32_t>(counts[index])) / static_cast<std::int32_t>(width)) * state.supplied;
		for (CompanyBuff const& buff : manager.at(state.works_as < manager.size() ? state.works_as : index).buffs) {
			if (buff.effect != effect) {
				continue;
			}
			const std::size_t scope_index = cache.find_unit_scope(buff.scope);
			for (ScopeWeight const& weight : weights) {
				if (weight.scope == scope_index) {
					sum += share * buff.value * weight.weight;
				}
			}
		}
	}
	return sum;
}

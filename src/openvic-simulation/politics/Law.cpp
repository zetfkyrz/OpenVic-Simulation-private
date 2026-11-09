#include "Law.hpp"

#include <algorithm>

#include "openvic-simulation/modifier/ModifierManager.hpp"
#include "openvic-simulation/politics/Ideology.hpp"
#include "openvic-simulation/politics/IssueManager.hpp"
#include "openvic-simulation/utility/Logger.hpp"

using namespace OpenVic;
using namespace OpenVic::NodeTools;

std::string_view OpenVic::law_effect_name(const law_effect_t effect) {
	switch (effect) {
#define OV_LAW_EFFECT_NAME(name) case law_effect_t::name: return #name;
		OV_LAW_EFFECTS(OV_LAW_EFFECT_NAME)
#undef OV_LAW_EFFECT_NAME
	default: return "?";
	}
}

namespace {
	callback_t<std::string_view> push_string(memory::vector<memory::string>& list) {
		return [&list](std::string_view value) -> bool {
			list.emplace_back(value);
			return true;
		};
	}
}

std::size_t LawManager::load_file(std::string_view source, ast::NodeCPtr root, ModifierManager const& modifier_manager) {
	std::size_t loaded = 0;
	expect_dictionary([&](std::string_view key, ast::NodeCPtr group_node) -> bool {
		if (key == "ideology_lean") {
			return expect_dictionary([this](std::string_view ideology, ast::NodeCPtr node) -> bool {
				return expect_fixed_point([this, ideology](fixed_point_t lean) -> bool {
					ideology_lean[memory::string { ideology }] = std::clamp(lean, -fixed_point_t::_1, fixed_point_t::_1);
					return true;
				})(node);
			})(group_node);
		}
		if (key != "law_group") {
			spdlog::warn_s("Unknown key \"{}\" in {}, skipped (only law_group and ideology_lean)", key, source);
			return true;
		}
		LawGroup group;
		group.source = memory::string { source };
		struct variant_node_t {
			memory::string identifier;
			ast::NodeCPtr node;
		};
		memory::vector<variant_node_t> variant_nodes;
		const bool ok = expect_dictionary_keys_and_default(
			[source](std::string_view unknown, ast::NodeCPtr) -> bool {
				spdlog::warn_s("Unknown key \"{}\" in a law group of {}", unknown, source);
				return true;
			},
			"id", ONE_EXACTLY, expect_identifier_or_string([&group](std::string_view id) -> bool {
				group.identifier = memory::string { id };
				return true;
			}),
			"column", ONE_EXACTLY, expect_identifier_or_string([&group](std::string_view column) -> bool {
				group.column = memory::string { column };
				return true;
			}),
			"order", ZERO_OR_ONE, expect_int64([&group](int64_t order) -> bool {
				group.order = static_cast<std::int32_t>(order);
				return true;
			}),
			"name", ZERO_OR_ONE, expect_identifier_or_string([&group](std::string_view name) -> bool {
				group.name_key = memory::string { name };
				return true;
			}),
			"icon", ZERO_OR_ONE, expect_identifier_or_string([&group](std::string_view icon) -> bool {
				group.icon = memory::string { icon };
				return true;
			}),
			"replaces", ZERO_OR_ONE, expect_list(expect_identifier_or_string(push_string(group.replaces))),
			"variant", ONE_OR_MORE, [&variant_nodes, source](ast::NodeCPtr node) -> bool {
				memory::string identifier;
				expect_dictionary_keys_and_default(
					key_value_success_callback,
					"id", ONE_EXACTLY, expect_identifier_or_string([&identifier](std::string_view id) -> bool {
						identifier = memory::string { id };
						return true;
					})
				)(node);
				if (identifier.empty()) {
					spdlog::warn_s("A law variant without an id in {}, skipped", source);
					return true;
				}
				variant_nodes.push_back({ std::move(identifier), node });
				return true;
			}
		)(group_node);
		if (!ok || group.identifier.empty() || variant_nodes.empty()) {
			spdlog::warn_s("A law group of {} is skipped: it needs an id, a column and variants", source);
			return true;
		}
		if (std::any_of(groups.begin(), groups.end(), [&group](LawGroup const& other) { return other.identifier == group.identifier; })) {
			spdlog::warn_s("The law group \"{}\" of {} is loaded already, skipped", group.identifier, source);
			return true;
		}
		const std::size_t group_index = groups.size();
		std::size_t order = 0;
		for (variant_node_t const& entry : variant_nodes) {
			if (get_variant(entry.identifier) != nullptr) {
				spdlog::warn_s("The law \"{}\" of {} is loaded already, skipped", entry.identifier, source);
				continue;
			}
			ModifierValue values;
			RuleSet rules;
			law_effects_t effects {};
			bool is_default = false;
			fixed_point_t lean = fixed_point_t::_0;
			memory::vector<memory::string> start_from, inherit;
			memory::string name_key;
			bool effects_ok = true;
			const bool variant_ok = expect_dictionary_keys_and_default(
				[source, &entry](std::string_view unknown, ast::NodeCPtr) -> bool {
					spdlog::warn_s("Unknown key \"{}\" in the law \"{}\" of {}", unknown, entry.identifier, source);
					return true;
				},
				"id", ONE_EXACTLY, success_callback,
				"name", ZERO_OR_ONE, expect_identifier_or_string([&name_key](std::string_view name) -> bool {
					name_key = memory::string { name };
					return true;
				}),
				"default", ZERO_OR_ONE, expect_bool(assign_variable_callback(is_default)),
				"lean", ZERO_OR_ONE, expect_fixed_point(assign_variable_callback(lean)),
				"modifier", ZERO_OR_ONE, expect_dictionary(modifier_manager.expect_base_country_modifier(values)),
				"rules", ZERO_OR_ONE, RuleSet::expect_rule_set(move_variable_callback(rules)),
				"start_from", ZERO_OR_ONE, expect_list(expect_identifier_or_string(push_string(start_from))),
				"inherit", ZERO_OR_ONE, expect_list(expect_identifier_or_string(push_string(inherit))),
				"effects", ZERO_OR_ONE, expect_dictionary([&effects, &effects_ok, &entry, source](std::string_view name, ast::NodeCPtr node) -> bool {
					for (std::size_t index = 0; index < LAW_EFFECT_COUNT; ++index) {
						if (law_effect_name(static_cast<law_effect_t>(index)) == name) {
							return expect_fixed_point(assign_variable_callback(effects[index]))(node);
						}
					}
					spdlog::warn_s("Unknown effect \"{}\" of the law \"{}\" of {}", name, entry.identifier, source);
					effects_ok = false;
					return true;
				})
			)(entry.node);
			if (!variant_ok || !effects_ok) {
				spdlog::warn_s("The law \"{}\" of {} is skipped, see the messages above", entry.identifier, source);
				continue;
			}
			LawVariant& variant = variants.emplace_back(entry.identifier, std::move(values));
			variant.index = variants.size() - 1;
			variant.group_index = group_index;
			variant.order = order++;
			variant.is_default = is_default;
			variant.lean = std::clamp(lean, -fixed_point_t::_1, fixed_point_t::_1);
			variant.rules = std::move(rules);
			variant.effects = effects;
			variant.start_from = std::move(start_from);
			variant.inherit = std::move(inherit);
			variant.name_key = std::move(name_key);
			variant.source = memory::string { source };
			group.variants.push_back(variant.index);
			loaded++;
		}
		if (group.variants.empty()) {
			spdlog::warn_s("The law group \"{}\" of {} has no usable variant, skipped", group.identifier, source);
			return true;
		}
		groups.push_back(std::move(group));
		return true;
	})(root);
	return loaded;
}

std::size_t LawManager::resolve(IssueManager const& issue_manager, IdeologyManager const& ideology_manager) {
	std::size_t problems = 0;
	lean_by_ideology.assign(ideology_manager.get_ideologies().size(), fixed_point_t::_0);
	for (Ideology const& ideology : ideology_manager.get_ideologies()) {
		const auto it = ideology_lean.find(ideology.get_identifier());
		if (it != ideology_lean.end()) {
			lean_by_ideology[type_safe::get(ideology.index)] = it->second;
		} else if (!groups.empty()) {
			spdlog::warn_s("The ideology \"{}\" has no lean for the laws (ideology_lean of ov_laws), 0 is taken", ideology.get_identifier());
			problems++;
		}
	}
	for (LawVariant& variant : variants) {
		variant.inherited_modifiers.clear();
		variant.inherited_rules.clear();
		for (memory::string const& key : variant.inherit) {
			if (Reform const* const reform = issue_manager.get_reform_by_identifier(key); reform != nullptr) {
				variant.inherited_modifiers.push_back(reform);
				variant.inherited_rules.push_back(&reform->rules);
			} else if (PartyPolicy const* const policy = issue_manager.get_party_policy_by_identifier(key); policy != nullptr) {
				variant.inherited_modifiers.push_back(policy);
				variant.inherited_rules.push_back(&policy->rules);
			} else {
				spdlog::warn_s("The law \"{}\" ({}) inherits \"{}\", which is no reform or party policy of Vic2", variant.get_identifier(), variant.source, key);
				problems++;
			}
		}
	}
	for (LawGroup const& group : groups) {
		std::size_t defaults = 0;
		for (const std::size_t index : group.variants) {
			defaults += variants[index].is_default ? 1 : 0;
		}
		if (defaults != 1) {
			spdlog::warn_s("The law group \"{}\" ({}) has {} default variants (one is needed, the first is taken)", group.identifier, group.source, defaults);
			problems++;
		}
		for (memory::string const& vic2_group : group.replaces) {
			if (issue_manager.get_reform_group_by_identifier(vic2_group) == nullptr
				&& issue_manager.get_party_policy_group_by_identifier(vic2_group) == nullptr) {
				spdlog::warn_s("The law group \"{}\" ({}) replaces \"{}\", which is no reform group or party policy group of Vic2", group.identifier,
					group.source, vic2_group);
				problems++;
			}
		}
	}
	return problems;
}

fixed_point_t LawManager::get_approval(const std::size_t ideology_index, LawVariant const& variant) const {
	const fixed_point_t lean = ideology_index < lean_by_ideology.size() ? lean_by_ideology[ideology_index] : fixed_point_t::_0;
	const fixed_point_t distance = lean > variant.lean ? lean - variant.lean : variant.lean - lean;
	return fixed_point_t::_1 - distance / 2;
}

LawVariant const* LawManager::get_variant(std::string_view identifier) const {
	for (LawVariant const& variant : variants) {
		if (variant.get_identifier() == identifier) {
			return &variant;
		}
	}
	return nullptr;
}

bool LawManager::replaces(std::string_view vic2_group) const {
	for (LawGroup const& group : groups) {
		if (std::find(group.replaces.begin(), group.replaces.end(), vic2_group) != group.replaces.end()) {
			return true;
		}
	}
	return false;
}

LawVariant const& LawManager::get_default_variant(LawGroup const& group) const {
	for (const std::size_t index : group.variants) {
		if (variants[index].is_default) {
			return variants[index];
		}
	}
	return variants[group.variants.front()];
}

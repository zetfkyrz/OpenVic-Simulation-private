#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <string_view>

#include "openvic-simulation/core/memory/String.hpp"
#include "openvic-simulation/core/memory/Vector.hpp"
#include "openvic-simulation/core/object/FixedPoint.hpp"
#include "openvic-simulation/dataloader/NodeTools.hpp"
#include "openvic-simulation/modifier/Modifier.hpp"
#include "openvic-simulation/politics/RuleSet.hpp"
#include "openvic-simulation/types/OrderedContainers.hpp"

namespace OpenVic {
	struct IdeologyManager;
	struct IssueManager;
	struct ModifierManager;

	// The laws of a country (docs/LAWS_DESIGN.md): groups of laws, one variant of every group in force. A variant is a modifier of the country (the
	// effects of Vic2), rules (Vic2), the modifiers and rules of the Vic2 reforms and party policies it takes over (inherit), and the effects of OpenVic
	// below, summed over the laws of the country and read by the mechanics. The Vic2 reform and party policy groups a law group replaces have no effect
	// of their own any more. Nothing is hardcoded: ov_data/common/ov_laws/*.txt.

	// The effects of OpenVic (shares: 0.2 is +20 %; a flag is 1)
#define OV_LAW_EFFECTS(X) \
	X(army_attack) /* the attack and the defence of the regiments */ \
	X(army_defence) \
	X(army_home_bonus) /* more attack and defence on the national land of the country */ \
	X(military_wage) /* the pay of the soldiers and the officers */ \
	X(manpower) /* the most men of the regular regiments */ \
	X(mobilisation_size) /* the pool of the mobilisation */ \
	X(mobilisation_impact) /* the cost of the mobilisation to the economy */ \
	X(bureaucrat_efficiency) /* how well the bureaucrats collect the taxes */ \
	X(bureaucrat_promotion) /* how many pops become bureaucrats */ \
	X(corruption) /* the share of the collected taxes the bureaucrats keep */ \
	X(bureaucrat_wage) /* the pay of the bureaucrats */ \
	X(no_foreign_trade) /* 1: no trade with other countries */ \
	X(vote_poor) /* the weight of the vote of the poor, the middle and the rich strata in the laws (the distribution of power) */ \
	X(vote_middle) \
	X(vote_rich) \
	X(tax_poor) /* the income tax of the poor, the middle and the rich strata (taxation: graduated, per capita...) */ \
	X(tax_middle) \
	X(tax_rich) \
	X(militancy_growth) /* how fast the militancy and the consciousness of the pops grow (police, security, free speech) */ \
	X(consciousness_growth) \
	X(rgo_owner_share) /* the share of the owners of the RGOs (land reform) */ \
	X(no_emigration) /* 1: nobody leaves the country (closed borders) */ \
	X(no_immigration) /* 1: nobody comes from abroad (closed borders, migration controls) */ \
	X(colonial_points) /* the colonial points (colonisation: -1 none at all) */ \
	X(free_slaves) /* the share of the slaves that become free every month (slavery banned) */ \
	X(transport_room_add) /* regiments every transport carries more (naval model) */ \
	X(warship_room_add) /* regiments every warship that carries any carries more */ \
	X(assimilation) /* how fast the unaccepted cultures assimilate (citizenship; -1 none) */

	enum struct law_effect_t : std::uint8_t {
#define OV_LAW_EFFECT_ENUM(name) name,
		OV_LAW_EFFECTS(OV_LAW_EFFECT_ENUM)
#undef OV_LAW_EFFECT_ENUM
		COUNT
	};
	static constexpr std::size_t LAW_EFFECT_COUNT = static_cast<std::size_t>(law_effect_t::COUNT);
	using law_effects_t = std::array<fixed_point_t, LAW_EFFECT_COUNT>;
	[[nodiscard]] std::string_view law_effect_name(law_effect_t effect);

	struct LawVariant : Modifier {
		std::size_t index = 0; // in the manager
		std::size_t group_index = 0;
		std::size_t order = 0; // in its group, from the old to the new
		bool is_default = false; // the variant of a country the start keys do not choose
		fixed_point_t lean = fixed_point_t::_0; // -1 the most reactionary .. 1 the most radical: the ideologies of the same lean like it
		RuleSet rules;
		law_effects_t effects {};
		memory::vector<memory::string> start_from; // Vic2 reforms, party policies, government types that choose it at the start
		memory::vector<memory::string> inherit; // Vic2 reforms and party policies whose modifiers and rules it takes
		memory::vector<Modifier const*> inherited_modifiers; // resolved after the issues are loaded
		memory::vector<RuleSet const*> inherited_rules;
		memory::string name_key;
		memory::string source;

		LawVariant(std::string_view new_identifier, ModifierValue&& new_values)
			: Modifier { new_identifier, std::move(new_values), modifier_type_t::REFORM } {}
		LawVariant(LawVariant&&) = default;

		[[nodiscard]] fixed_point_t get_effect(const law_effect_t effect) const {
			return effects[static_cast<std::size_t>(effect)];
		}
	};

	struct LawGroup {
		memory::string identifier;
		memory::string column; // power, economy or human_rights (the window)
		std::int32_t order = 0; // in its column
		memory::string name_key;
		memory::string icon;
		memory::vector<memory::string> replaces; // the Vic2 reform and party policy groups it replaces
		memory::vector<std::size_t> variants; // indices in the manager, in their order
		memory::string source;
	};

	struct LawManager {
	private:
		std::deque<LawVariant> variants; // the countries point to them
		memory::vector<LawGroup> groups;
		string_map_t<fixed_point_t> ideology_lean; // ideology_lean = { reactionary = -1 ... } of the files
		memory::vector<fixed_point_t> lean_by_ideology; // by the index of the ideology (resolve)

	public:
		// A file of law groups (law_group = { ... } one or more). A broken group or variant is skipped with a warning. Returns the variants loaded.
		std::size_t load_file(std::string_view source, ast::NodeCPtr root, ModifierManager const& modifier_manager);
		// After the issues of Vic2: the inherited modifiers and rules, the checks. Returns the problems found.
		std::size_t resolve(IssueManager const& issue_manager, IdeologyManager const& ideology_manager);
		// How much the ideology likes the variant: 1 - |lean of the ideology - lean of the variant| / 2 (0..1)
		[[nodiscard]] fixed_point_t get_approval(std::size_t ideology_index, LawVariant const& variant) const;

		[[nodiscard]] std::deque<LawVariant> const& get_variants() const {
			return variants;
		}
		[[nodiscard]] memory::vector<LawGroup> const& get_groups() const {
			return groups;
		}
		[[nodiscard]] LawVariant const* get_variant(std::string_view identifier) const;
		[[nodiscard]] bool empty() const {
			return groups.empty();
		}
		// Is this Vic2 reform group or party policy group replaced by the laws
		[[nodiscard]] bool replaces(std::string_view vic2_group) const;
		[[nodiscard]] LawVariant const& get_default_variant(LawGroup const& group) const;
	};
}

#pragma once

#include <cstdint>
#include <string_view>

#include "openvic-simulation/core/memory/String.hpp"
#include "openvic-simulation/core/memory/Vector.hpp"

namespace OpenVic {
	struct CountryDefinition;
	struct CountryInstance;
	struct InstanceManager;
	struct State;

	// The unions of the nations (version 103, stage D8 of docs/PLAN_DIPLOMACY_VIC2.md): the union of a culture group (union = GER of
	// common/cultures.txt: Germany, Italy, Scandinavia...). The decision of Vic2 (form_germany...): a great power of the group with
	// UNION_FORM_PRESTIGE prestige, at peace, not a subject, all the land with a core of the union owned by it, the members of its sphere or
	// its subjects, takes the name, the flag and the colour of the union (the country stays the same inside: its tag in the saves, its armies,
	// laws and technologies), gets a core on the land of the union cores and accepts all the cultures of the group, +UNION_FORM_PRESTIGE_GAIN.
	// The small countries of the group in its sphere or its subjects are asked to join it (an AI agrees UNION_JOIN_CHANCE percent of the time,
	// as in Vic2; a refusal: the relation -100, its prestige +1); later the union invites them (action join_union, the AI agrees with a relation
	// of UNION_JOIN_RELATION). The war of unification (cb unification_casus_belli of Vic2): a great power of the group (or the union) against a
	// country of another culture group that holds land with the union cores, no casus belli and no infamy; such states are goals in the peace.
	// The peace term "hegemony" (clear_union_sphere of Vic2): the members of the culture group leave the sphere of the loser.
	namespace Unions {
		// The union of the culture group of the country (nullptr: none)
		CountryDefinition const* get_union_definition(CountryInstance const& country);
		// The country of the union definition (its instance)
		CountryInstance& get_union_instance(InstanceManager& instance_manager, CountryDefinition const& union_definition);
		// Who is the union: the country that formed it, or the country of the union itself while it exists (nullptr: nobody)
		CountryInstance* get_union_holder(InstanceManager& instance_manager, CountryDefinition const& union_definition);
		// The state has land with a core of the union (its instance)
		bool is_union_state(State const& state, CountryInstance const& union_instance);

		// The requirements of the decision for the interface: (text, met)
		struct requirement_t {
			memory::string text;
			bool met = false;
			CountryInstance const* example = nullptr; // a country that holds the land of the union (its name is added by the interface)
		};
		memory::vector<requirement_t> get_form_requirements(InstanceManager& instance_manager, CountryInstance& country);
		// Why the country cannot form its union now (empty: it can)
		memory::string get_form_block_reason(InstanceManager& instance_manager, CountryInstance& country);
		bool form_union(InstanceManager& instance_manager, CountryInstance& country);

		// May the member join the union of the holder (the action join_union)
		bool may_join(InstanceManager& instance_manager, CountryInstance const& holder, CountryInstance const& member);
		void join_union(InstanceManager& instance_manager, CountryInstance& holder, CountryInstance& member);

		// The war of unification: the attacker may use it against the target (empty: it may)
		std::string_view get_unification_block_reason(InstanceManager& instance_manager, CountryInstance const& attacker, CountryInstance const& target);

		// The countries of the culture group of the winner in the sphere of the loser (the peace term "hegemony")
		memory::vector<CountryInstance*> get_hegemony_members(InstanceManager& instance_manager, CountryInstance const& winner, CountryInstance const& loser);

		// Once a month: the AI forms its union, asks the members, fights the wars of unification
		void ai_monthly(InstanceManager& instance_manager);
	}
}

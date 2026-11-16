#pragma once

#include <cstdint>
#include <string_view>

#include "openvic-simulation/core/memory/String.hpp"
#include "openvic-simulation/country/CountryInstance.hpp"

namespace OpenVic {
	struct InstanceManager;
	struct OpenVicDiplomacyDefines;
	struct State;

	// The subjects (version 101, stage D6 of docs/PLAN_DIPLOMACY_VIC2.md, the table "Субъекты"): a country under an overlord. The kinds:
	// - puppet: no foreign policy of its own (no alliances, no wars but the war of independence), pays a share of its income;
	// - protectorate: may not attack its overlord, the overlord defends it;
	// - colonial subject: may not attack its overlord, the overlord joins all its wars; it pays a share of the growth of its treasury (the
	//   overlord sets it), its RGOs make more and its factories less, the technologies its overlord knows are cheaper;
	// - crown land: may not attack its overlord, the overlord joins all its wars, the technologies are cheaper;
	// - dominion: may go to war against its overlord, nobody joins its wars.
	// A puppet and a dominion may fight a war of independence (no casus belli needed); the peace term "independence" frees them. The overlord
	// may annex a subject (its consent: SUBJECT_ANNEX_MIN_YEARS years a subject and a relation of SUBJECT_ANNEX_MIN_RELATION), release it, give
	// it more autonomy (the next kind of the table), change the payments of a colonial subject or a crown land and draw them into its wars.
	// The numbers: ov_diplomacy SUBJECT_*
	namespace Subjects {
		using subject_type_t = CountryInstance::subject_type_t;

		// "puppet", "protectorate", "colonial", "crown_land", "dominion" ("" for none): the keys of the interface
		std::string_view get_type_key(subject_type_t type);

		// The rules of the kinds
		bool overlord_defends(subject_type_t type); // the overlord joins a war declared on the subject
		bool overlord_joins_attacks(subject_type_t type); // the overlord joins a war the subject declares
		bool may_attack_overlord(subject_type_t type); // the war of independence
		bool has_foreign_policy(subject_type_t type); // false: no alliances, no wars of its own
		bool tribute_is_changeable(subject_type_t type); // the overlord raises and lowers the payments

		std::int32_t get_relation_bonus(OpenVicDiplomacyDefines const& defines, subject_type_t type);
		std::int32_t get_default_tribute_pct(OpenVicDiplomacyDefines const& defines, subject_type_t type);
		// The limits of the payments the overlord may set (a colonial subject: percent of the growth of the treasury)
		std::int32_t get_min_tribute_pct(OpenVicDiplomacyDefines const& defines, subject_type_t type);
		std::int32_t get_max_tribute_pct(OpenVicDiplomacyDefines const& defines, subject_type_t type);
		// The kind of more autonomy (the next row of the table; NONE after the dominion: the subject is free)
		subject_type_t get_more_autonomy(subject_type_t type);

		// The country becomes a subject of the overlord (its own subjects are freed, a puppet leaves its alliances)
		void make_subject(InstanceManager& instance_manager, CountryInstance& subject, CountryInstance& overlord, subject_type_t type, bool notify = true);
		void set_type(InstanceManager& instance_manager, CountryInstance& subject, subject_type_t type);
		// The subject is free (the news to both when notify)
		void release(InstanceManager& instance_manager, CountryInstance& subject, bool notify = true);
		// Why the overlord cannot annex the subject now (empty: it can)
		memory::string get_annex_block_reason(InstanceManager const& instance_manager, CountryInstance const& overlord, CountryInstance const& subject);
		// The land of the subject goes to the overlord, its armies and fleets are disbanded; false if it cannot be annexed
		bool annex(InstanceManager& instance_manager, CountryInstance& overlord, CountryInstance& subject);

		// The release of nations (version 102, the tab "release nations" of the politics window): a nation that does not exist, whose core is the
		// capital of a state of the country (not the state of its capital), gets these states, whole; it may stay a subject of the country (a
		// colonial subject only out of colonial land; a subject releases only free nations). RELEASE_NATION_PRESTIGE and RELEASE_NATION_INFAMY
		// of defines.lua, the relation of the two RELEASE_NATION_RELATION, the share of the treasury of the population that leaves
		struct releasable_t {
			CountryInstance* nation = nullptr;
			memory::vector<State*> states;
			std::size_t provinces = 0;
			std::int64_t population = 0;
			bool colonial = false; // some of its land is a colony or a protectorate
		};
		// In the order of the countries
		memory::vector<releasable_t> get_releasable_nations(CountryInstance const& country);
		// Why the country cannot release the nation as this kind (NONE: free) now (empty: it can)
		std::string_view get_release_block_reason(CountryInstance const& country, releasable_t const& entry, subject_type_t type);
		bool release_nation(InstanceManager& instance_manager, CountryInstance& country, CountryInstance& nation, subject_type_t type);

		// The subjects and the overlords of the history (Vic2 vassal and union: puppets, substate: crown lands) at the start of the game
		void setup_history(InstanceManager& instance_manager);
		// Every day: the payments to the overlords; a subject or an overlord that does not exist any more ends the bond. A colonial subject also
		// gives its overlord SUBJECT_COLONIAL_RAW_SURPLUS_PCT of its surplus of raw goods (version 102): its RGOs put that share of their output
		// aside instead of selling it (the share of every good from the production and the demand of the country of the day before), and it
		// goes to the national stockpile of the overlord
		void daily_update(InstanceManager& instance_manager);
		// Every month: the discontent of the subjects (their relation with the overlord goes down, SUBJECT_DISCONTENT_PER_MONTH)
		void monthly_update(InstanceManager& instance_manager);
	}
}

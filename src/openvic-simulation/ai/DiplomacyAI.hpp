#pragma once

#include <cstdint>

#include "openvic-simulation/core/object/FixedPoint.hpp"

namespace OpenVic {
	struct CountryInstance;
	struct InstanceManager;

	// The diplomacy of the AI (version 91, docs/DIPLOMACY.md): the threats of the neighbours and the great powers, the alliances against them, the
	// processes of the relations, the interest of the great powers in the countries (land and sea) and their goals every AI_GOAL_YEARS. And the help
	// of the AI for the player (the goods of the stockpile, the borders without troops in a war). All the numbers: ov_diplomacy
	namespace DiplomacyAI {
		// Once a month (CountryAI::monthly_update calls it first)
		void monthly_update(InstanceManager& instance_manager);

		// Whether the receiver (an AI country) accepts an alliance with the proposer: > 0 yes
		std::int32_t alliance_acceptance(InstanceManager const& instance_manager, CountryInstance const& proposer, CountryInstance const& receiver);

		// The interest of a country in another (0..1): 1 for a land neighbour, else the closeness by the sea x the share of its navy
		fixed_point_t interest(InstanceManager const& instance_manager, CountryInstance const& country, CountryInstance const& other);

		// How much the AI wants a war against a land neighbour (0: no war): weaker, worse relations, the target of its goal
		fixed_point_t war_target_score(InstanceManager const& instance_manager, CountryInstance const& country, CountryInstance const& target);

		// The help of the AI for the player, once a week (the InstanceManager calls it)
		void weekly_assist(InstanceManager& instance_manager);
	}
}

#pragma once

namespace OpenVic {
	struct InstanceManager;

	/* A simple AI for the countries that are not played (F1), once a month:
	 * - builds a factory in a state with the best expected profit when the treasury can afford it,
	 * - keeps recruiting regiments while at war and below the size of the army it wants,
	 * - declares a war against a weak neighbour now and then (the aggression is a setting),
	 * - the armies at war are led by the automatic plan of the war manager (fronts), generals and admirals are assigned
	 *   by the war manager too.
	 * Nothing is random: the "dice" are a hash of the country and the date, so that a game can be replayed. */
	namespace CountryAI {
		// All the parameters of the AI are in the group ov_ai of common/openvic_defines.lua (defines/OpenVicDefines.hpp)
		void monthly_update(InstanceManager& instance_manager);
	}
}

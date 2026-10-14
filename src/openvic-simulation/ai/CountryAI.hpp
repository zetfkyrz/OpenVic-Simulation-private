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
		// Yearly chance of an eligible country to look for a war, per cent. 0 switches the AI wars off.
		inline constexpr int WAR_CHANCE_PERCENT_PER_YEAR = 25;
		// The target must be weaker than this share of the military power of the attacker, per cent (300 for the player)
		inline constexpr int MAX_TARGET_POWER_PERCENT = 60;
		inline constexpr int MAX_TARGET_POWER_PERCENT_PLAYER = 30;
		// Only the strongest countries by total rank start wars
		inline constexpr int MAX_RANK_FOR_WARS = 30;
		// A new factory must pay back within this many years
		inline constexpr int FACTORY_PAYBACK_YEARS = 8;
		// Regiments per owned province an AI country wants to have at war, and in peace, per mille
		inline constexpr int WAR_REGIMENTS_PER_PROVINCE_PERMILLE = 300;
		inline constexpr int MAX_RECRUITS_PER_MONTH = 3;

		void monthly_update(InstanceManager& instance_manager);
	}
}

#pragma once

namespace OpenVic {
	struct InstanceManager;
	struct PopDeps;

	/* Changes of the population that happen once a month, after the economy of the day:
	 * - growth or decline of every pop by how well its life needs are met (starvation shrinks it),
	 * - assimilation of pops with an unaccepted culture into the primary culture of the country,
	 * - demotion of unemployed craftsmen to farmers / labourers and promotion of unemployed farmers / labourers to
	 *   craftsmen when the factories of the state lack workers,
	 * - internal migration from provinces with high unemployment and poor life needs to better neighbouring provinces.
	 * Nothing is random: the fractions of people that cannot be moved yet are kept in the pops until they add up.
	 * The money of the pops stays where it is. */
	namespace PopDynamics {
		// Share of the life needs that must be met for the pop not to shrink, per cent. The economy gives the poor pops only
		// about 40-75% of their life needs at the start of the game (see the POP report of the headless runner), so the
		// breakeven is low; raise it when the economy of the pops is calibrated.
		inline constexpr int GROWTH_BREAKEVEN_PERCENT = 35;
		// Yearly growth of a pop with all life needs met, and yearly decline of a starving pop
		inline constexpr int GROWTH_PERMILLE_PER_YEAR = 25;
		inline constexpr int DECLINE_PERMILLE_PER_YEAR = 60;
		// Share of an unaccepted pop that is assimilated each month, per mille (twice as much inside a culture group)
		inline constexpr int ASSIMILATION_PERMILLE_PER_MONTH = 1;
		// Share of the unemployed craftsmen demoted / unemployed farmers and labourers promoted each month
		inline constexpr int PROMOTION_PERCENT_PER_MONTH = 4;
		// Share of a pop that migrates each month, per mille
		inline constexpr int MIGRATION_PERMILLE_PER_MONTH = 3;
		// A province attracts migrants when its score (0..1) is better by this much, per cent
		inline constexpr int MIGRATION_SCORE_MARGIN_PERCENT = 8;

		void monthly_update(InstanceManager& instance_manager, PopDeps const& pop_deps);
	}
}

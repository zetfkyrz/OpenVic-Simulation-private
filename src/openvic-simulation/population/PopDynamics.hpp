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
		// All the parameters (growth, assimilation, promotion, migration) are in the group ov_population of
		// common/openvic_defines.lua (defines/OpenVicDefines.hpp)
		void monthly_update(InstanceManager& instance_manager, PopDeps const& pop_deps);
	}
}

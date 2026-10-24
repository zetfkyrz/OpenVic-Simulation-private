#pragma once

namespace OpenVic {
	struct InstanceManager;

	/* Inventions (E1), once a month. The Victoria 2 scripts of the inventions (limit, chance) cannot be evaluated by the
	 * simulation yet, so an invention is discovered by a simple rule: it becomes possible when a technology it is
	 * associated with is researched, and then progresses each month by a rate that grows with the literacy of the country
	 * (about 1.5 years for a country with 50% literacy). New technologies and inventions are written to the notification
	 * log for the interface. */
	namespace ResearchDynamics {
		// The rates are in the group ov_research of common/openvic_defines.lua
		void monthly_update(InstanceManager& instance_manager);
	}
}

#pragma once

namespace OpenVic {
	struct CountryInstance;
	struct InstanceManager;
	struct LawVariant;

	// The passing of the laws (docs/LAWS_DESIGN.md, the way (b)), once a month: a proposed law passes with a chance by the support of the voters
	// over the law in force, a stall gives it up; the AI proposes the change its voters like the most
	struct LawDynamics {
		static void monthly_update(InstanceManager& instance_manager);
		// A law of a lower lean than the law in force (back towards the reaction) passes only under a reactionary or fascist government
		[[nodiscard]] static bool may_pass(CountryInstance const& country, LawVariant const& variant, LawVariant const* current);
	};
}

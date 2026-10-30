#pragma once

#include "openvic-simulation/research/ArmyTechnology.hpp"
#include "openvic-simulation/research/Invention.hpp"
#include "openvic-simulation/research/Technology.hpp"

namespace OpenVic {
	struct ResearchManager {
	private:
		TechnologyManager PROPERTY_REF(technology_manager);
		InventionManager PROPERTY_REF(invention_manager);
		// The technologies of the army of OpenVic (ov_data/common/ov_technology), independent of the technologies of Vic2
		ArmyTechnologyManager PROPERTY_REF(army_technology_manager);
	};
}

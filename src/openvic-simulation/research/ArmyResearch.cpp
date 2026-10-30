#include "ArmyResearch.hpp"

#include <algorithm>
#include <limits>

#include "openvic-simulation/InstanceManager.hpp"
#include "openvic-simulation/DefinitionManager.hpp"
#include "openvic-simulation/country/CountryInstance.hpp"
#include "openvic-simulation/defines/OpenVicDefines.hpp"

using namespace OpenVic;

namespace {
	ArmyTechnologyManager const& technologies_of(InstanceManager const& instance_manager) {
		return instance_manager.definition_manager.get_research_manager().get_army_technology_manager();
	}

	OpenVicResearchDefines const& defines_of(InstanceManager const& instance_manager) {
		return instance_manager.definition_manager.get_define_manager().get_openvic_defines().research;
	}
}

bool OpenVic::are_army_prerequisites_met(ArmyTechnologyManager const& manager, CountryInstance const& country, ArmyTechnology const& technology) {
	for (memory::vector<std::size_t> const& block : manager.get_prerequisite_indices(technology)) {
		bool any = false;
		for (const std::size_t index : block) {
			any |= country.has_army_technology(static_cast<std::uint32_t>(index));
		}
		if (!any) {
			return false;
		}
	}
	return true;
}

ArmyResearchProgress const* OpenVic::find_army_research(ArmyTechnologyManager const& manager, CountryInstance const& country, const std::size_t branch_index) {
	for (ArmyResearchProgress const& progress : country.get_army_research()) {
		if (manager.at(progress.technology).branch_index == branch_index) {
			return &progress;
		}
	}
	return nullptr;
}

tech_state_t OpenVic::get_army_technology_state(ArmyTechnologyManager const& manager, CountryInstance const& country, ArmyTechnology const& technology) {
	if (country.has_army_technology(static_cast<std::uint32_t>(technology.index))) {
		return tech_state_t::RESEARCHED;
	}
	for (ArmyResearchProgress const& progress : country.get_army_research()) {
		if (progress.technology == technology.index) {
			return tech_state_t::RESEARCHING;
		}
	}
	return are_army_prerequisites_met(manager, country, technology) ? tech_state_t::AVAILABLE : tech_state_t::NOT_AVAILABLE;
}

army_research_check_t OpenVic::can_start_army_research(InstanceManager const& instance_manager, CountryInstance const& country, const std::size_t technology_index) {
	ArmyTechnologyManager const& manager = technologies_of(instance_manager);
	if (technology_index >= manager.size()) {
		return army_research_check_t::NO_SUCH_TECHNOLOGY;
	}
	ArmyTechnology const& technology = manager.at(technology_index);
	if (defines_of(instance_manager).army_research_civilised_only != 0 && !country.is_civilised()) {
		return army_research_check_t::NOT_ALLOWED;
	}
	switch (get_army_technology_state(manager, country, technology)) {
	case tech_state_t::RESEARCHED:
		return army_research_check_t::ALREADY_RESEARCHED;
	case tech_state_t::RESEARCHING:
		return army_research_check_t::ALREADY_RESEARCHING;
	case tech_state_t::NOT_AVAILABLE:
		return army_research_check_t::PREREQUISITES;
	case tech_state_t::AVAILABLE:
		break;
	}
	if (find_army_research(manager, country, technology.branch_index) != nullptr) {
		return army_research_check_t::SLOT_BUSY;
	}
	return army_research_check_t::OK;
}

bool OpenVic::start_army_research(InstanceManager& instance_manager, CountryInstance& country, const std::size_t technology_index) {
	if (can_start_army_research(instance_manager, country, technology_index) != army_research_check_t::OK) {
		return false;
	}
	country.get_army_research_mutable().push_back(ArmyResearchProgress { static_cast<std::uint32_t>(technology_index), 0 });
	return true;
}

bool OpenVic::cancel_army_research(InstanceManager&, CountryInstance& country, const std::size_t technology_index) {
	memory::vector<ArmyResearchProgress>& research = country.get_army_research_mutable();
	const auto it = std::find_if(research.begin(), research.end(), [technology_index](ArmyResearchProgress const& progress) -> bool {
		return progress.technology == technology_index;
	});
	if (it == research.end()) {
		return false;
	}
	research.erase(it);
	return true;
}

bool OpenVic::complete_army_technology(InstanceManager& instance_manager, CountryInstance& country, const std::size_t technology_index) {
	ArmyTechnologyManager const& manager = technologies_of(instance_manager);
	if (technology_index >= manager.size()) {
		return false;
	}
	ArmyTechnology const& technology = manager.at(technology_index);
	if (!country.mark_army_technology_researched(static_cast<std::uint32_t>(technology_index))) {
		return false;
	}
	std::erase_if(country.get_army_research_mutable(), [technology_index](ArmyResearchProgress const& progress) -> bool {
		return progress.technology == technology_index;
	});
	for (memory::string const& key : technology.unlocks) {
		country.grant_unlock(key);
	}
	if (technology.modifier.has_value()) {
		country.add_unlock_modifier(*technology.modifier);
	}
	return true;
}

std::int32_t OpenVic::get_army_research_days_left(InstanceManager const& instance_manager, CountryInstance const& country, const std::size_t technology_index) {
	ArmyTechnologyManager const& manager = technologies_of(instance_manager);
	if (technology_index >= manager.size() || country.has_army_technology(static_cast<std::uint32_t>(technology_index))) {
		return 0;
	}
	const std::int32_t days = manager.at(technology_index).research_days;
	for (ArmyResearchProgress const& progress : country.get_army_research()) {
		if (progress.technology == technology_index) {
			return std::max(0, days - progress.days_done);
		}
	}
	return days;
}

std::size_t OpenVic::choose_ai_army_technology(InstanceManager const& instance_manager, CountryInstance const& country, const std::size_t branch_index) {
	ArmyTechnologyManager const& manager = technologies_of(instance_manager);
	std::size_t best = std::numeric_limits<std::size_t>::max();
	for (ArmyTechnology const& technology : manager.get_technologies()) {
		if (technology.branch_index != branch_index || can_start_army_research(instance_manager, country, technology.index) != army_research_check_t::OK) {
			continue;
		}
		best = technology.index; // the first available in the order of the files
		break;
	}
	return best;
}

void OpenVic::update_army_research(InstanceManager& instance_manager) {
	ArmyTechnologyManager const& manager = technologies_of(instance_manager);
	if (manager.size() == 0) {
		return;
	}
	OpenVicResearchDefines const& research_defines = defines_of(instance_manager);

	for (CountryInstance& country : instance_manager.get_country_instance_manager().get_country_instances()) {
		if (!country.exists() || country.is_rebel_country()) {
			continue;
		}

		// The start technologies of the civilised (also for a country that became civilised later)
		if (!country.are_army_start_technologies_given() && country.is_civilised()) {
			for (ArmyTechnology const& technology : manager.get_technologies()) {
				if (technology.start == "civilised") {
					complete_army_technology(instance_manager, country, technology.index);
				}
			}
			country.set_army_start_technologies_given(true);
		}

		// A day of research
		memory::vector<std::uint32_t> done;
		for (ArmyResearchProgress& progress : country.get_army_research_mutable()) {
			progress.days_done++;
			if (progress.days_done >= manager.at(progress.technology).research_days) {
				done.push_back(progress.technology);
			}
		}
		for (const std::uint32_t technology_index : done) {
			complete_army_technology(instance_manager, country, technology_index);
		}

		// The AI starts a technology in every free slot
		if (country.is_ai() && research_defines.army_research_ai_enabled != 0) {
			for (std::size_t branch_index = 0; branch_index < manager.get_branches().size(); ++branch_index) {
				if (find_army_research(manager, country, branch_index) != nullptr) {
					continue;
				}
				const std::size_t choice = choose_ai_army_technology(instance_manager, country, branch_index);
				if (choice != std::numeric_limits<std::size_t>::max()) {
					start_army_research(instance_manager, country, choice);
				}
			}
		}
	}
}

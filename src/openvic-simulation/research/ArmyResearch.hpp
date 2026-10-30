#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "openvic-simulation/research/ArmyTechnology.hpp"

namespace OpenVic {
	struct ArmyResearchProgress;
	struct CountryInstance;
	struct InstanceManager;

	// The research of the army technologies of a country (docs/WAR_ASSUMPTIONS.md, section 5): one slot for a branch, a technology takes its
	// research_days, nothing is paid. When it is done the keys of the technology are granted (CountryInstance::grant_unlock) and its modifier
	// is added to the country. The civilised countries get the technologies with start = civilised when they become civilised (at the start
	// of the game for those that are). The AI picks the first available technology (the order of the files) when its slot is free.

	enum struct army_research_check_t : std::uint8_t {
		OK,
		NO_SUCH_TECHNOLOGY,
		NOT_ALLOWED, // the country is not civilised (ov_research.ARMY_RESEARCH_CIVILISED_ONLY)
		ALREADY_RESEARCHED,
		ALREADY_RESEARCHING,
		SLOT_BUSY, // another technology of the branch is being researched
		PREREQUISITES
	};

	// All the blocks of the prerequisites have a researched technology (a technology without prerequisites is always available)
	[[nodiscard]] bool are_army_prerequisites_met(ArmyTechnologyManager const& manager, CountryInstance const& country, ArmyTechnology const& technology);
	[[nodiscard]] tech_state_t get_army_technology_state(ArmyTechnologyManager const& manager, CountryInstance const& country, ArmyTechnology const& technology);
	[[nodiscard]] army_research_check_t can_start_army_research(InstanceManager const& instance_manager, CountryInstance const& country, std::size_t technology_index);
	// The technology being researched in the branch (nullptr if none) and the days done
	[[nodiscard]] ArmyResearchProgress const* find_army_research(ArmyTechnologyManager const& manager, CountryInstance const& country, std::size_t branch_index);

	bool start_army_research(InstanceManager& instance_manager, CountryInstance& country, std::size_t technology_index);
	// The progress is lost
	bool cancel_army_research(InstanceManager& instance_manager, CountryInstance& country, std::size_t technology_index);
	// The technology is researched at once, with its unlocks and modifier (also for the start technologies, the console and the tests)
	bool complete_army_technology(InstanceManager& instance_manager, CountryInstance& country, std::size_t technology_index);
	// The days left of a technology being researched (research_days if it is not)
	[[nodiscard]] std::int32_t get_army_research_days_left(InstanceManager const& instance_manager, CountryInstance const& country, std::size_t technology_index);
	// The technology the AI would start in the branch (SIZE_MAX if none): the first available one in the order of the files
	[[nodiscard]] std::size_t choose_ai_army_technology(InstanceManager const& instance_manager, CountryInstance const& country, std::size_t branch_index);

	// Every day (before the modifier sums): the start technologies of the countries that became civilised, the progress of the research,
	// the AI choosing a technology for a free slot
	void update_army_research(InstanceManager& instance_manager);
}

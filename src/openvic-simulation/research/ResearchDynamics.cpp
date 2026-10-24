#include "ResearchDynamics.hpp"

#include <unordered_map>

#include "openvic-simulation/DefinitionManager.hpp"
#include "openvic-simulation/InstanceManager.hpp"
#include "openvic-simulation/country/CountryInstance.hpp"
#include "openvic-simulation/research/Invention.hpp"
#include "openvic-simulation/research/ResearchManager.hpp"
#include "openvic-simulation/research/Technology.hpp"
#include "openvic-simulation/utility/Logger.hpp"

using namespace OpenVic;

void ResearchDynamics::monthly_update(InstanceManager& instance_manager) {
	OpenVicResearchDefines const& defines = instance_manager.definition_manager.get_define_manager().get_openvic_defines().research;
	TechnologyManager const& technologies = instance_manager.definition_manager.get_research_manager().get_technology_manager();
	const Date today = instance_manager.get_today();

	// Progress toward each invention of each country, and the number of technologies seen last time
	auto& progress = instance_manager.get_research_state().progress;
	auto& known_technologies = instance_manager.get_research_state().known_technologies;

	for (CountryInstance& country : instance_manager.get_country_instance_manager().get_country_instances()) {
		if (!country.exists() || country.is_rebel_country()) {
			continue;
		}

		// Monthly chance of an invention: a base share plus a share of the literacy of the country
		const fixed_point_t rate =
			(fixed_point_t(defines.invention_base_chance_per_month.numerator)
				+ country.get_average_literacy() * defines.invention_literacy_chance.numerator) / Rate::DENOMINATOR;

		std::int32_t unlocked = 0;
		for (Technology const& technology : technologies.get_technologies()) {
			if (!country.is_technology_unlocked(technology)) {
				continue;
			}
			++unlocked;
			for (Invention const& invention : technology.get_inventions()) {
				if (country.is_invention_unlocked(invention)) {
					continue;
				}
				const std::uint64_t key = (static_cast<std::uint64_t>(type_safe::get(country.index)) << 32)
					| static_cast<std::uint64_t>(type_safe::get(invention.index));
				fixed_point_t& value = progress[key];
				value += rate;
				if (value >= fixed_point_t::_1) {
					country.unlock_invention(invention);
					progress.erase(key);
					instance_manager.get_notification_log().add(today, &country, "invention", invention.get_identifier());
				}
			}
		}

		const auto it = known_technologies.find(&country);
		if (it != known_technologies.end() && unlocked > it->second) {
			instance_manager.get_notification_log().add(today, &country, "technology");
		}
		known_technologies[&country] = unlocked;
	}
}

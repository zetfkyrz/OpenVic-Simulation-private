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
	TechnologyManager const& technologies = instance_manager.definition_manager.get_research_manager().get_technology_manager();
	const Date today = instance_manager.get_today();

	// Progress toward each invention of each country, and the number of technologies seen last time
	static std::unordered_map<std::uint64_t, fixed_point_t> progress;
	static std::unordered_map<CountryInstance const*, std::int32_t> known_technologies;

	for (CountryInstance& country : instance_manager.get_country_instance_manager().get_country_instances()) {
		if (!country.exists() || country.is_rebel_country()) {
			continue;
		}

		const fixed_point_t rate = (BASE_PROGRESS_PERCENT + country.get_average_literacy() * LITERACY_PROGRESS_PERCENT) / 100;

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

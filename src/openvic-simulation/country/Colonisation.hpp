#pragma once

#include <cstdint>

#include "openvic-simulation/core/memory/Vector.hpp"
#include "openvic-simulation/core/object/Date.hpp"
#include "openvic-simulation/core/object/FixedPoint.hpp"
#include "openvic-simulation/core/memory/String.hpp"

namespace OpenVic {
	struct CountryInstance;
	struct InstanceManager;
	struct ProvinceInstance;

	/* Colonisation (F2) in a simple form: a country with free colonial points starts a project in an unowned land province
	 * that it can reach (a land neighbour, or a coastal province in the sea range of its ports); the project uses one
	 * COLONIZATION_INTEREST_COST_INITIAL points (less for a land neighbour) while it runs and finishes after the colonisation duration of the defines (the shortest of the projects wins
	 * the province, the others are cancelled). The province becomes a colony of the country. Colonial points come from the
	 * modifiers of the country, from the naval bases and from the ships. */
	struct ColonisationManager {
		struct Project {
			CountryInstance* country;
			ProvinceInstance* province;
			std::int32_t days_done = 0;
			std::int32_t days_total = 1;
			fixed_point_t cost; // colonial points the project uses while it runs
		};

	private:
		memory::vector<Project> projects;

	public:
		// Countries below this place of the ranking cannot colonise (ov_colonisation.MAX_COLONIAL_RANK of the defines)
		static std::size_t get_max_colonial_rank(InstanceManager const& instance_manager);

		memory::vector<Project> const& get_projects() const {
			return projects;
		}
		// All the colonial points the country has, and those in use by its projects and colonies (maintenance)
		fixed_point_t get_total_points(InstanceManager const& instance_manager, CountryInstance const& country) const;
		fixed_point_t get_used_points(InstanceManager const& instance_manager, CountryInstance const& country) const;
		// Colonial points a new project in the province would use
		fixed_point_t get_project_cost(InstanceManager& instance_manager, CountryInstance& country, ProvinceInstance& province) const;
		// Empty if the country can start a project in the province, otherwise the reason
		memory::string get_block_reason(InstanceManager& instance_manager, CountryInstance& country, ProvinceInstance& province) const;
		bool start_project(InstanceManager& instance_manager, CountryInstance& country, ProvinceInstance& province);
		bool cancel_project(CountryInstance const& country, ProvinceInstance const& province);
		// Whether the project of the country in the province is going on
		Project const* find_project(CountryInstance const& country, ProvinceInstance const& province) const;
		// The unowned land provinces the country could colonise (reach only, points are not checked)
		memory::vector<ProvinceInstance*> get_candidates(InstanceManager& instance_manager, CountryInstance& country) const;

		// The same provinces as get_candidates minus those get_block_reason refuses (for the AI, which needs no reasons): the reachable
		// seas are worked out once instead of once for every province
		memory::vector<ProvinceInstance*> get_startable_candidates(InstanceManager& instance_manager, CountryInstance& country) const;

		void tick(InstanceManager& instance_manager);
	};
}

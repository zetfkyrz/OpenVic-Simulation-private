#pragma once

#include <cstdint>

#include "openvic-simulation/core/memory/String.hpp"
#include "openvic-simulation/core/memory/Vector.hpp"
#include "openvic-simulation/core/object/FixedPoint.hpp"

namespace OpenVic {
	struct CountryInstance;
	struct InstanceManager;
	struct ProvinceInstance;
	struct Reform;
	struct ReformGroup;

	/* The political life of the countries, once a month:
	 * - every pop gets a distribution of ideologies from its type, consciousness, militancy and literacy,
	 * - from the ideologies follow its support of the options of every reform and party policy (each option has a
	 *   place on a line from conservative to radical, an ideology supports the options near its own place),
	 * - the pops that may vote (by the voting reforms of the country) vote for the active parties of their ideologies,
	 * - countries with elections hold them when the term of the government ends: the party with most votes rules,
	 * - the upper house gets the ideology distribution of the voters (by the rules of the country).
	 * The Victoria 2 scripts that weigh these things (conditional weights) cannot be evaluated by the simulation yet, so
	 * everything is a simple transparent model, the constants are in PoliticsDynamics.cpp. */
	namespace PoliticsDynamics {
		void monthly_update(InstanceManager& instance_manager);

		struct ReformStatus {
			bool can_enact = false;
			memory::string reason; // why it cannot be enacted (empty if it can)
			fixed_point_t support; // share of the support of the group the option has (0..1)
			fixed_point_t upper_house; // acceptance of the upper house (0..1)
		};

		// Support of the reform among the people of the country and whether the country can enact it now
		ReformStatus get_reform_status(InstanceManager const& instance_manager, CountryInstance const& country, Reform const& reform);
		// Support (0..1) of every option of the group in the country, indexed like group.get_reforms()
		void get_group_support(CountryInstance const& country, ReformGroup const& group, fixed_point_t* out_supports);
	}
}

namespace OpenVic {
	/* Unrest (D4), once a month:
	 * - militancy of the pops grows with hunger, unemployment and war exhaustion and decays otherwise, consciousness
	 *   grows with literacy,
	 * - a province where many pops are militant rises: a rebel army appears and fights the owner (the rebel country is at
	 *   war with the owner while the army exists), the rebels may take the province,
	 * - when the rebels are beaten the militancy of the province falls, and the province returns to its owner. */
	namespace RebelDynamics {
		void monthly_update(InstanceManager& instance_manager);

		// Pops with militancy above this level (of 10) are ready to rise
		inline constexpr int MILITANT_LEVEL = 6;
		// Militant people needed for a rising, and how many fill one rebel regiment
		inline constexpr int MIN_MILITANT_PEOPLE = 30000;
		inline constexpr int PEOPLE_PER_REBEL_REGIMENT = 12000;
		inline constexpr int MAX_REBEL_REGIMENTS = 6;
		// Share of the people of the province that must be militant, per cent
		inline constexpr int MIN_MILITANT_PERCENT = 15;
		// Days before a province can rise again, and days after which the government of an AI country puts a rising down
		inline constexpr int UPRISING_COOLDOWN_DAYS = 365;
		inline constexpr int AI_SUPPRESSION_DAYS = 90;

		struct Uprising {
			ProvinceInstance const* province;
			std::int32_t regiments;
			fixed_point_t strength; // 0..1 of the rebel army
		};
		// Rebel armies in the provinces of the country
		memory::vector<Uprising> get_uprisings(InstanceManager const& instance_manager, CountryInstance const& country);
	}
}

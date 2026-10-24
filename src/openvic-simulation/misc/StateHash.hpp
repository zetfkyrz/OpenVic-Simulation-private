#pragma once

#include <cstdint>

namespace OpenVic {
	struct InstanceManager;

	/* A 64 bit hash of the state of a game that the simulation changes: the date, the pops (size, cash, militancy,
	 * consciousness, literacy), the provinces (owner, controller), the countries (treasury, prestige, wars), the factories,
	 * the prices, the armies and fleets, the wars and the colonisation. Two games that were played the same way must give
	 * the same hash (the tests of determinism and of save games compare it). */
	namespace StateHash {
		std::uint64_t compute(InstanceManager& instance_manager);
	}
}

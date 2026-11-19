#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

#include "openvic-simulation/core/memory/Vector.hpp"
#include "openvic-simulation/core/object/Date.hpp"
#include "openvic-simulation/core/object/FixedPoint.hpp"

namespace OpenVic {
	struct CountryInstance;
	struct InstanceManager;
	struct State;

	// The crises (version 105, stage D7 of docs/PLAN_DIPLOMACY_VIC2.md), as in Vic2:
	// - a flashpoint is a state with the core of another country (a claim) or of a nation without a country (a liberation); its tension grows
	//   every month (CRISIS_TENSION_*), slower in the land of a great power (RANK_n_TENSION_DECAY of defines.lua) and not while its owner is
	//   at war; with 100 of tension a crisis starts there (CRISIS_BASE_CHANCE a month, CRISIS_COOLDOWN_MONTHS after the last one);
	// - the great powers that are not at war and not tired (CRISIS_INTEREST_WAR_EXHAUSTION_LIMIT) are interested: each backs the claim (the
	//   attackers), the owner (the defenders) or stays out; the first backer of a side leads it (a great power owner or claimant leads its own);
	// - after CRISIS_BACKER_DAYS a side without a leader loses (the claim without one fades away, the owner without one gives the state);
	// - while both sides have a leader the temperature grows every day, faster the more of the interested took a side
	//   (CRISIS_TEMPERATURE_PARTICIPANT_FACTOR) and +CRISIS_TEMPERATURE_ON_MOBILIZE for every side member that mobilises;
	// - a leader may give in: the other side wins (the state goes to the claimant, a liberated nation gets it); the winners get prestige
	//   (CRISIS_WINNER_PRESTIGE_FACTOR_*), the losers like the winner less (CRISIS_WINNER_RELATIONS_IMPACT), the interested who stayed out
	//   lose prestige (CRISIS_DID_NOT_TAKE_SIDE_PRESTIGE_FACTOR_*);
	// - at 100 of temperature the great war: the leader of the attackers declares war on the owner for the state, all the sides join; the
	//   peace of a great war costs GW_WARSCORE_COST_MOD of the war score.
	struct CrisisManager {
		struct crisis_t {
			bool active = false;
			State* state = nullptr;
			CountryInstance* owner = nullptr;
			CountryInstance* claimant = nullptr;
			bool liberation = false; // the claimant has no country of its own
			CountryInstance* attacker_leader = nullptr;
			CountryInstance* defender_leader = nullptr;
			memory::vector<CountryInstance*> attackers; // the great powers that back the claim (with the leader)
			memory::vector<CountryInstance*> defenders; // the great powers that back the owner (with the leader)
			memory::vector<CountryInstance*> interested; // the great powers that may still take a side
			std::uint32_t interested_at_start = 0;
			memory::vector<CountryInstance*> mobilised; // the members whose mobilisation heated the crisis already
			memory::vector<CountryInstance*> ai_decided; // the AI great powers that chose already
			fixed_point_t temperature;
			Date start;
		};
		struct flashpoint_t {
			State* state = nullptr;
			CountryInstance* claimant = nullptr;
			bool liberation = false;
		};

	private:
		crisis_t crisis;
		Date last_end;
		bool any_crisis_yet = false;

		void start(InstanceManager& instance_manager, flashpoint_t const& flashpoint);
		void finish(InstanceManager& instance_manager, bool attackers_win);
		void fizzle(InstanceManager& instance_manager);
		void great_war(InstanceManager& instance_manager);
		// +1: the AI great power backs the attackers, -1 the defenders, 0 it stays out
		int ai_choice(InstanceManager& instance_manager, CountryInstance const& power) const;
		void join(CountryInstance& country, bool attackers_side);

	public:
		crisis_t const& get_crisis() const {
			return crisis;
		}
		Date get_last_end() const {
			return last_end;
		}
		bool had_crisis() const {
			return any_crisis_yet;
		}
		// The claimant of the state (a claim before a liberation), none if the state is no flashpoint
		static std::optional<flashpoint_t> get_flashpoint(State& state);
		// The flashpoints with the most tension, in the order of the tension (the interface)
		memory::vector<flashpoint_t> get_hottest(InstanceManager& instance_manager, std::size_t count) const;

		std::string_view get_back_block_reason(CountryInstance const& country, bool attackers_side) const;
		bool back(InstanceManager& instance_manager, CountryInstance& country, bool attackers_side);
		// The leader of a side gives in: the other side wins
		bool concede(InstanceManager& instance_manager, CountryInstance& country);
		// The temperature as it grows a day now (the interface)
		fixed_point_t get_daily_temperature(InstanceManager const& instance_manager) const;

		// Once a month: the tension of the flashpoints, a new crisis
		void monthly_update(InstanceManager& instance_manager);
		// Every day: the sides, the temperature, the AI, the great war
		void daily_update(InstanceManager& instance_manager);
		// The crisis starts in the state now; its temperature is set (the self-tests)
		bool force_start(InstanceManager& instance_manager, State& state);
		void set_temperature_for_tests(const fixed_point_t temperature) {
			crisis.temperature = temperature;
		}
	};
}

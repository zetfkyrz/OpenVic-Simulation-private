#pragma once

#include <cstdint>
#include <span>
#include <unordered_map>
#include <unordered_set>

#include "openvic-simulation/core/memory/Vector.hpp"
#include "openvic-simulation/core/object/Date.hpp"
#include "openvic-simulation/core/object/FixedPoint.hpp"
#include "openvic-simulation/types/UniqueId.hpp"

namespace OpenVic {
	struct CountryInstance;
	struct InstanceManager;
	struct ProvinceInstance;
	struct State;

	// A war between two sides. The first country of each side is its leader.
	// The war goal is always conquest: the side that holds more enemy land wins more at the peace table.
	struct War {
		std::uint32_t id = 0;
		Date start_date;
		memory::vector<CountryInstance*> attackers;
		memory::vector<CountryInstance*> defenders;

		// Score gathered in battles (capped), the occupation score is calculated from the map each tick
		fixed_point_t attacker_battle_score;
		fixed_point_t defender_battle_score;
		// Total war score of each side, 0..100
		fixed_point_t attacker_score;
		fixed_point_t defender_score;

		bool is_attacker(CountryInstance const& country) const;
		bool is_defender(CountryInstance const& country) const;
		bool has_participant(CountryInstance const& country) const {
			return is_attacker(country) || is_defender(country);
		}
		memory::vector<CountryInstance*> const& get_side(bool attackers_side) const {
			return attackers_side ? attackers : defenders;
		}
		fixed_point_t get_score(bool attackers_side) const {
			return attackers_side ? attacker_score : defender_score;
		}
	};

	// A front is a line of provinces (usually along the border with an enemy) that armies are assigned to.
	// Assigned armies are moved automatically: in DEFEND mode they spread along the line, in ATTACK mode they advance
	// into the enemy provinces next to the line, one target per army.
	struct Front {
		enum struct mode_t : std::uint32_t { DEFEND = 0, ATTACK = 1 };

		std::uint32_t id = 0;
		CountryInstance* country = nullptr;
		// Enemy the front was generated against, nullptr for a hand drawn front
		CountryInstance const* enemy = nullptr;
		mode_t mode = mode_t::DEFEND;
		memory::vector<ProvinceInstance*> provinces;
		memory::vector<unique_id_t> army_ids;
	};

	struct WarManager {
		enum struct peace_type_t : std::uint32_t {
			WHITE_PEACE = 0, // nothing changes hands
			TAKE_STATES = 1, // the proposer's side takes states it occupies, up to its war score
			GIVE_STATES = 2  // the proposer gives the other side the states it occupies, up to the other side's war score
		};

		struct Siege {
			CountryInstance const* besieger = nullptr;
			std::int32_t days = 0;
			std::int32_t days_required = 0;
			// Progress in days, grows faster with more regiments (days is its whole part)
			fixed_point_t progress;
		};

		// Tunable constants
		static constexpr std::int32_t TRUCE_DAYS = 5 * 365;
		static constexpr std::int32_t SIEGE_BASE_DAYS = 3;
		static constexpr std::int32_t SIEGE_DAYS_PER_FORT_LEVEL = 5;
		static constexpr std::int32_t CAPITAL_PROVINCE_WEIGHT = 5;
		static constexpr std::int32_t MAX_BATTLE_SCORE = 25;

	private:
		memory::vector<War> wars;
		std::uint32_t next_war_id = 1;

		struct Truce {
			CountryInstance const* first;
			CountryInstance const* second;
			Date until;
		};
		memory::vector<Truce> truces;

		std::unordered_map<ProvinceInstance const*, Siege> sieges;
		std::unordered_set<unique_id_t> fought_today;

		// Ports with an enemy fleet in front of them and no fleet of the owner, and the share of ports of each country
		std::unordered_set<ProvinceInstance const*> blockaded_ports;
		std::unordered_map<CountryInstance const*, fixed_point_t> blockade_shares;

		memory::vector<Front> fronts;
		std::uint32_t next_front_id = 1;
		// Countries whose fronts and army assignments are managed automatically
		std::unordered_set<CountryInstance const*> auto_plan_countries;

		Front* find_front(std::uint32_t id);
		void update_fronts(InstanceManager& instance_manager);
		void update_auto_plan(InstanceManager& instance_manager);
		void order_front_armies(InstanceManager& instance_manager, Front& front);

		War* find_war(std::uint32_t id);
		War* find_war_between(CountryInstance const& first, CountryInstance const& second);

		void update_combat(InstanceManager& instance_manager);
		void update_sieges(InstanceManager& instance_manager);
		void update_scores(InstanceManager& instance_manager);
		void update_army_recovery(InstanceManager& instance_manager);
		void update_attrition(InstanceManager& instance_manager);
		void update_naval_combat(InstanceManager& instance_manager);
		void update_blockades(InstanceManager& instance_manager);
		void update_leaders(InstanceManager& instance_manager);

		void end_war(InstanceManager& instance_manager, std::uint32_t war_id);
		void transfer_states(InstanceManager& instance_manager, War const& war, bool winners_are_attackers);

	public:
		bool is_blockaded(ProvinceInstance const& port) const {
			return blockaded_ports.contains(&port);
		}
		// Share (0..1) of the ports of the country that are blockaded
		fixed_point_t get_blockade_share(CountryInstance const& country) const {
			const auto it = blockade_shares.find(&country);
			return it != blockade_shares.end() ? it->second : fixed_point_t::_0;
		}

		std::span<const War> get_wars() const {
			return wars;
		}
		War const* get_war_by_id(std::uint32_t id) const;
		// Wars the country takes part in
		memory::vector<War const*> get_wars_of(CountryInstance const& country) const;

		bool has_truce(CountryInstance const& first, CountryInstance const& second, Date today) const;
		// Siege progress of the province, nullptr if there is none
		Siege const* get_siege(ProvinceInstance const& province) const;

		// Weight of a country's province for the war score (a capital is worth several ordinary provinces)
		static std::int32_t get_province_weight(ProvinceInstance const& province);
		// Number of days a siege of the province takes
		static std::int32_t get_siege_days_required(ProvinceInstance const& province);

		bool can_declare_war(CountryInstance const& attacker, CountryInstance const& defender, Date today) const;
		// Starts a war, the allies of both sides who are not allied to the enemy join in automatically
		bool declare_war(InstanceManager& instance_manager, CountryInstance& attacker, CountryInstance& defender);

		// The decision of the other side's leader. This is the single place that has to be replaced by real AI/player choice.
		bool would_accept_peace(
			InstanceManager const& instance_manager, War const& war, bool proposer_is_attacker, peace_type_t peace_type
		) const;
		// Proposes peace in the war, the proposer must take part in it. Returns true if the peace was signed.
		bool propose_peace(
			InstanceManager& instance_manager, std::uint32_t war_id, CountryInstance& proposer, peace_type_t peace_type
		);

		std::span<const Front> get_fronts() const {
			return fronts;
		}
		Front const* get_front_by_id(std::uint32_t id) const;
		bool is_auto_plan_enabled(CountryInstance const& country) const {
			return auto_plan_countries.contains(&country);
		}

		// Creates a front for the country. With an enemy the border provinces of the country next to the enemy are
		// added in order along the border (auto front), without one the front starts empty to be drawn by hand.
		// Returns the id of the front, 0 on failure.
		std::uint32_t create_front(InstanceManager& instance_manager, CountryInstance& country, CountryInstance const* enemy);
		bool delete_front(CountryInstance const& country, std::uint32_t front_id);
		// Adds a province of the country to the end of the front (or removes it if it is already in the line)
		bool toggle_front_province(CountryInstance const& country, std::uint32_t front_id, ProvinceInstance& province);
		bool set_front_mode(CountryInstance const& country, std::uint32_t front_id, Front::mode_t mode);
		bool set_front_army(
			InstanceManager& instance_manager, CountryInstance const& country, std::uint32_t front_id, unique_id_t army_id,
			bool assigned
		);
		// Assigns every army of the country that is not on a front yet to the front
		bool assign_free_armies(InstanceManager& instance_manager, CountryInstance const& country, std::uint32_t front_id);
		void set_auto_plan(CountryInstance const& country, bool enabled);

		void tick(InstanceManager& instance_manager);
	};
}

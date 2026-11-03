#pragma once

#include <cstdint>
#include <span>
#include <unordered_map>
#include <unordered_set>

#include "openvic-simulation/core/memory/Vector.hpp"
#include "openvic-simulation/core/object/Date.hpp"
#include "openvic-simulation/core/object/FixedPoint.hpp"
#include "openvic-simulation/defines/OpenVicDefines.hpp"
#include "openvic-simulation/military/BattleFormulas.hpp"
#include "openvic-simulation/military/DivisionStats.hpp"
#include "openvic-simulation/types/UniqueId.hpp"

namespace OpenVic {
	struct CountryInstance;
	struct InstanceManager;
	struct ProvinceInstance;
	struct State;

	// What the attacker wants from the war. A conquered state of the war goal costs half of the war score at the peace table.
	struct WarGoal {
		enum struct type_t : std::uint32_t {
			NONE = 0,      // no goal: the war can only be won by occupation
			CONQUER_STATE = 1, // take the state (state_province is any province of it)
			HUMILIATE = 2, // prestige: the winner gains and the loser loses it
			REPARATIONS = 3 // the loser pays a part of its treasury
		};
		type_t type = type_t::NONE;
		ProvinceInstance const* state_province = nullptr;
	};

	// Terms of a peace, built by a country before the offer is sent
	struct PeaceTerms {
		std::uint32_t war_id = 0;
		// true: the proposer is losing and gives the states listed (the enemy occupies them), false: the proposer demands them
		bool concede = false;
		memory::vector<ProvinceInstance const*> states; // one province of each state
		bool reparations = false;
		bool humiliate = false;
	};

	// The answer to the last offer of a country, shown in the peace window
	struct PeaceResponse {
		bool accepted = false;
		memory::string text;
		Date date;
	};

	// A peace offered to a country that has to answer it (the player)
	struct PeaceOffer {
		std::uint32_t war_id = 0;
		CountryInstance* from = nullptr;
		bool from_is_attacker = false;
		// The terms are from the point of view of the sender: concede = the sender gives the states
		PeaceTerms terms;
		Date date;
	};

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
		WarGoal goal; // of the attacker

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
		// The group of armies (the field marshal) that holds the front, 0 if none (B3, docs/COMMAND.md). A front of a group is commanded only while the plan
		// of the group for this front is active: the armies of it get no orders otherwise and the automatic plan does not touch it
		std::uint32_t group_id = 0;
	};

	// A land battle in a province (docs/WAR_ASSUMPTIONS.md, section 6), kept from day to day: which side attacks, the counter of the breakthrough rule
	// and the divisions that were in the front of the battle yesterday (they keep their places, the reserve takes the free ones)
	struct LandBattle {
		CountryInstance const* attacker = nullptr; // a country of the attacking side: the side is it and the countries that fight the other side with it
		battle_formulas::BreakthroughCounter counter;
		std::int32_t days = 0;
		memory::vector<std::uint32_t> front_divisions;
	};

	// What a day of a land battle came to (the dumps and the tests; the reports of the last day only)
	struct LandBattleReport {
		ProvinceInstance const* province = nullptr;
		CountryInstance const* attacker = nullptr;
		CountryInstance const* defender = nullptr;
		std::int32_t day = 0; // the day of the battle (1 for the first)
		std::uint32_t width = 0; // the ranks of the battle (the terrain)
		std::uint32_t attacker_front = 0, attacker_reserve = 0, defender_front = 0, defender_reserve = 0; // divisions
		std::uint32_t attacker_ranks = 0, defender_ranks = 0; // the ranks the divisions of the front fill
		fixed_point_t attacker_attack, defender_attack; // of the front, with the buffs and the multipliers (the attacker without its conditions)
		fixed_point_t conditions; // the conditions of the attacker (the terrain, the river, the night, the weather)
		fixed_point_t ratio; // R: the breakthrough of the attackers over the protection of the defenders
		fixed_point_t attacker_strength_lost, defender_strength_lost; // strength points
		fixed_point_t attacker_strength, defender_strength, attacker_max_strength, defender_max_strength; // of all the divisions of the battle, after the day
		fixed_point_t attacker_organisation, defender_organisation; // the average of the front divisions after the day (points)
		std::uint32_t attacker_retreated = 0, defender_retreated = 0; // divisions that left the battle today
		bool breakthrough = false; // the rule of the breakthrough made the defence retreat today
		bool ended = false;
		bool attacker_won = false;
	};

	// What a battle would come to (WarManager::estimate_battle; the same work out of a day as the battle, without changing the game)
	struct BattleEstimate {
		bool valid = false; // false: no attacking division
		fixed_point_t ratio; // R of the first day (1000 when nobody defends)
		fixed_point_t attacker_attack, defender_attack; // of the fronts
		fixed_point_t attacker_damage, defender_damage; // the damage the fronts take in a day
		fixed_point_t attacker_days, defender_days; // the days until the first division of the front has no organisation left (1000: never)
		std::uint32_t breakthrough_days = 0; // the breakthrough rule makes the defence retreat after these days (1 or BREAK_WEEK_DAYS), 0: not by R
		bool attacker_wins = false; // the defence leaves first (by R or by the organisation)
	};

	// What the land battles of a country came to since the start of the game (the dumps and the comparison of the battle models; not in the state hash)
	struct BattleStats {
		std::uint32_t battle_days = 0; // days a division (or a regiment of the earlier model) of the country fought, one per province and day
		std::uint32_t won = 0; // battles the enemy left (retreated or was destroyed)
		std::uint32_t lost = 0; // battles the country left
		fixed_point_t strength_lost; // strength points the regiments lost in battle (a point is ov_war.TEMPLATE_MEN_PER_STRENGTH men)
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

		// The parameters (truce, sieges, battles, recovery, attrition, ...) are in the groups ov_war and ov_peace of
		// common/openvic_defines.lua, set once when the instance is created
		void set_defines(OpenVicDefines const& new_defines) {
			defines = &new_defines;
		}
		OpenVicWarDefines const& get_war_defines() const {
			return defines->war;
		}
		OpenVicPeaceDefines const& get_peace_defines() const {
			return defines->peace;
		}

	private:
		OpenVicDefines const* defines = nullptr;
		memory::vector<War> wars;
		std::uint32_t next_war_id = 1;

		struct Truce {
			CountryInstance const* first;
			CountryInstance const* second;
			Date until;
		};
		memory::vector<Truce> truces;

		std::unordered_map<CountryInstance const*, PeaceTerms> peace_drafts;
		std::unordered_map<CountryInstance const*, PeaceResponse> last_responses;
		memory::vector<PeaceOffer> offers; // to AI controlled or player countries, the receiver is the other side of the war
		// Days a country wants to wait before it makes a new offer in the war: war id + country -> date
		std::unordered_map<std::uint64_t, Date> next_offer_date;
		void apply_peace_terms(InstanceManager& instance_manager, War const& war, bool proposer_is_attacker, PeaceTerms const& terms);
		void update_ai_peace(InstanceManager& instance_manager);

		std::unordered_map<ProvinceInstance const*, Siege> sieges;
		std::unordered_set<unique_id_t> fought_today;
		std::unordered_map<CountryInstance const*, BattleStats> battle_stats;
		std::unordered_map<ProvinceInstance const*, LandBattle> land_battles;
		memory::vector<LandBattleReport> battle_reports;
		// What the divisions are worth now (compute_live_division_stats): the division id -> the signature of what it depends on and the numbers.
		// Not a state of the game: the same numbers come out without it
		struct LiveStatsEntry {
			std::uint64_t signature = 0;
			DivisionStats stats;
		};
		mutable std::unordered_map<std::uint32_t, LiveStatsEntry> live_stats_cache;
		mutable std::uint64_t live_stats_computed = 0;

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
		// Spreads the divisions of the armies of the front over its line: while the front has fewer armies than sectors (the provinces
		// of the line, or the enemy provinces to attack), a division of the biggest idle army leaves it as an army of its own
		void spread_front_divisions(InstanceManager& instance_manager, Front& front);

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
		// What the division of the army is worth now (docs/WAR_ASSUMPTIONS.md, section 6), from the cache: it is computed again when the regiments of
		// the division, their men, their weapons, the general of the army, its template or the day (the modifiers and the technologies of the country
		// are summed every day) change
		DivisionStats const& get_live_division_stats(InstanceManager const& instance_manager, ArmyInstance const& army, DivisionInstance const& division) const;
		// What a battle of these armies (in any provinces: the support from several directions) against the enemies in the province would come to
		// (docs/WAR_ASSUMPTIONS.md, section 6). The same work out of a day as the battle: the width, the front, the live divisions, the buffs, the
		// generals, the supply, the fortification, the terrain, the river from where every army stands. Changes nothing (the cache of the numbers only).
		[[nodiscard]] BattleEstimate estimate_battle(
			InstanceManager const& instance_manager, std::span<ArmyInstance const* const> attacking_armies, ProvinceInstance const& target
		) const;
		// How many times get_live_division_stats computed the numbers (the tests of the cache)
		std::uint64_t get_live_stats_computed() const {
			return live_stats_computed;
		}
		std::span<const LandBattleReport> get_battle_reports() const {
			return battle_reports;
		}
		LandBattle const* get_land_battle(ProvinceInstance const& province) const {
			const auto it = land_battles.find(&province);
			return it != land_battles.end() ? &it->second : nullptr;
		}
		std::size_t get_land_battle_count() const {
			return land_battles.size();
		}

		BattleStats get_battle_stats(CountryInstance const& country) const {
			const auto it = battle_stats.find(&country);
			return it != battle_stats.end() ? it->second : BattleStats {};
		}

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
		std::int32_t get_province_weight(ProvinceInstance const& province) const;
		// Number of days a siege of the province takes
		std::int32_t get_siege_days_required(ProvinceInstance const& province) const;

		bool can_declare_war(CountryInstance const& attacker, CountryInstance const& defender, Date today) const;
		// Starts a war, the allies of both sides who are not allied to the enemy join in automatically
		bool declare_war(InstanceManager& instance_manager, CountryInstance& attacker, CountryInstance& defender);
		bool declare_war(
			InstanceManager& instance_manager, CountryInstance& attacker, CountryInstance& defender, WarGoal const& goal
		);

		// ---- Peace negotiations ----
		// The costs, the shares and the days of the peace talks are in the group ov_peace of the defines

		// Draft of the terms of a country (the player builds it in the peace window)
		PeaceTerms& get_peace_draft(CountryInstance const& country, std::uint32_t war_id);
		PeaceTerms const* find_peace_draft(CountryInstance const& country) const;
		void clear_peace_draft(CountryInstance const& country);
		// The war score a state costs when the side takes it from its owner (half for the war goal)
		fixed_point_t get_state_cost(War const& war, bool winners_are_attackers, ProvinceInstance const& state_province) const;
		// States that can be put in the terms: the enemy states the side holds a province of (and the goal state),
		// or, when conceding, the own states the enemy holds a province of
		memory::vector<ProvinceInstance const*> get_peace_candidates(War const& war, CountryInstance const& proposer, bool concede) const;
		// Total war score the terms cost
		fixed_point_t get_terms_cost(War const& war, CountryInstance const& proposer, PeaceTerms const& terms) const;
		// What the receiver of the offer says, with the reason (no side effects)
		bool evaluate_offer(
			InstanceManager const& instance_manager, War const& war, bool proposer_is_attacker, PeaceTerms const& terms,
			memory::string* reason = nullptr
		) const;
		// Sends the terms, applies them if the other side accepts. Returns whether the peace was signed; the answer is
		// kept in get_last_response.
		bool send_peace_offer(InstanceManager& instance_manager, CountryInstance& proposer, PeaceTerms const& terms);

		PeaceResponse const* get_last_response(CountryInstance const& country) const;

		// Offers waiting for the answer of the country
		memory::vector<PeaceOffer> get_offers_for(CountryInstance const& country) const;
		bool answer_peace_offer(InstanceManager& instance_manager, CountryInstance& country, std::uint32_t war_id, bool accept);

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
		// The group of the front (0 takes it from the group); the front has to be the country's
		bool set_front_group(CountryInstance const& country, std::uint32_t front_id, std::uint32_t group_id);
		// Does the front get orders: a front without a group always does, a front of a group only while its plan is active
		[[nodiscard]] bool is_front_commanded(Front const& front) const;
		// The armies of the front stop where they are (the moves are cancelled, the armies in a battle stay in it)
		void stop_front_armies(InstanceManager& instance_manager, std::uint32_t front_id);

		void tick(InstanceManager& instance_manager);
	};
}

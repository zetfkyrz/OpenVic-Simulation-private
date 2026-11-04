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

	// A piece of a line between two neighbouring provinces: ours (the side of the country) and the other one (the land of a neighbour, or the sea of a
	// coastal front)
	struct FrontEdge {
		ProvinceInstance* own = nullptr;
		ProvinceInstance* other = nullptr;

		bool operator==(FrontEdge const&) const = default;
	};

	// A front is a line along the border (docs/WAR_ASSUMPTIONS.md, section 6): the pieces of the border between the land the country controls and the land
	// held by the countries of the sector (the neighbour it was made against and, while they fight the country in the same war, his allies), or a coastal
	// front along the coast of the country next to a sea province. The provinces are ours on the line, in order along it. Assigned armies are moved
	// automatically: in DEFEND mode they spread along the line, in ATTACK mode they advance into the enemy provinces next to the line.
	struct Front {
		enum struct mode_t : std::uint32_t { DEFEND = 0, ATTACK = 1 };
		enum struct kind_t : std::uint32_t { BORDER = 0, COAST = 1 };

		std::uint32_t id = 0;
		CountryInstance* country = nullptr;
		// The neighbour the front was made against (BORDER), nullptr for a coastal front or a front without a line
		CountryInstance const* enemy = nullptr;
		mode_t mode = mode_t::DEFEND;
		kind_t kind = kind_t::BORDER;
		// The sea province of a coastal front
		ProvinceInstance const* sea = nullptr;
		memory::vector<FrontEdge> edges;
		memory::vector<ProvinceInstance*> provinces;
		memory::vector<unique_id_t> army_ids;
		// The group of armies (the field marshal) that holds the front, 0 if none (B3, docs/COMMAND.md). A front of a group is commanded only while the
		// group executes its plan: the armies of it get no orders otherwise and the automatic plan does not touch it
		std::uint32_t group_id = 0;
		// The player changed the pieces of the line (the edit mode): the line keeps them instead of the whole border
		bool edited = false;
	};

	// A line of the plan of a field marshal (docs/WAR_ASSUMPTIONS.md, section 6): an offensive (the enemy provinces to take, from a front of the
	// marshal), a line of defence (provinces of the country to hold and dig in) or a naval invasion (a port of the country and the coast to land on)
	struct PlanLine {
		enum struct type_t : std::uint32_t { OFFENSIVE = 0, DEFENCE = 1, NAVAL = 2 };

		std::uint32_t id = 0;
		CountryInstance* country = nullptr;
		std::uint32_t group_id = 0;
		type_t type = type_t::OFFENSIVE;
		std::uint32_t front_id = 0; // OFFENSIVE: the front it starts from
		ProvinceInstance* port = nullptr; // NAVAL: where the divisions embark
		// OFFENSIVE: the targets (the line of the offensive is their far border), DEFENCE: the provinces to hold, NAVAL: the coast to land on
		memory::vector<ProvinceInstance*> provinces;
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
		// The chance of the victory of the attackers, 0..1 (battle_formulas::win_chance; 1 when nobody defends, 0 without attackers)
		fixed_point_t win_chance;
	};

	// Does the executor of an offensive attack with the estimate (P5, docs/WAR_ASSUMPTIONS.md, section 6): the chance of the victory at least PLAN_WIN_CAREFUL
	// (aggression 1, careful: 80 %), PLAN_WIN_BALANCED (2: 65 %), PLAN_WIN_AGGRESSIVE (3: 50 %); never without attackers
	[[nodiscard]] bool offensive_attacks(std::uint32_t aggression, BattleEstimate const& estimate, OpenVicWarDefines const& defines);

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
		memory::vector<PlanLine> plan_lines;
		std::uint32_t next_plan_line_id = 1;
		// What the lines of the fronts of the field marshals depend on (the controllers of the land, the sides of the wars), seen the last time they were
		// made (docs/WAR_ASSUMPTIONS.md, section 6, P3). Not a state of the game: a load replays the days from the start and gets the same
		std::uint64_t border_signature = 0;
		// Countries whose fronts and army assignments are managed automatically
		std::unordered_set<CountryInstance const*> auto_plan_countries;

		Front* find_front(std::uint32_t id);
		PlanLine* find_plan_line(std::uint32_t id);
		// The lines of the fronts of the field marshals follow the border (P3): made again when a province changes its controller or a war its sides
		void update_front_lines(InstanceManager& instance_manager);
		// The armies of the group of the front (an order of a field marshal) are on it: the ones of the group join, the others leave, an army moving by a
		// manual order joins when it stops
		void join_group_armies(Front& front);
		// The divisions of the order of a field marshal along its line (P4, docs/WAR_ASSUMPTIONS.md, section 6): split into divisions of their own, a share of
		// them for every province by its weight (FRONT_MIN_WEIGHT plus the enemy divisions next to it), the ones without a place to the nearest province
		// that lacks divisions; integrity 0 never moves a placed division, 1 when its province has FRONT_REBALANCE_THRESHOLD more than its share, 2 at one
		void distribute_divisions(InstanceManager& instance_manager, Front& front, std::uint32_t integrity);
		// The executor of the offensives from the front (P5): the targets of the zone next to the line, best first (R, nearer to the line of the plan);
		// for each the free divisions next to it (the minimum to hold stays) attack when offensive_attacks says so. The line of the plan is not passed:
		// the zone holds the targets and the land between
		void execute_offensive(
			InstanceManager& instance_manager, Front& front, std::span<ProvinceInstance* const> zone, std::span<ProvinceInstance* const> targets,
			std::uint32_t aggression
		);
		// The pieces of the line of the front as the border is now (an edited front keeps its pieces that are still on the border and gets the pieces of the
		// provinces taken from the other side and of our neighbours of the ones the enemy took)
		void rebuild_front_edges(InstanceManager& instance_manager, Front& front);
		// The lines of the fronts and of the plans lose what does not exist any more (fronts of groups that are gone, provinces taken from a line)
		void update_plan_lines(InstanceManager& instance_manager);
		void update_fronts(InstanceManager& instance_manager);
		void update_auto_plan(InstanceManager& instance_manager);
		// The zone (when limited) holds the provinces the armies may attack: the offensives of a field marshal
		void order_front_armies(InstanceManager& instance_manager, Front& front, std::span<ProvinceInstance* const> zone = {}, bool limited = false);
		// Spreads the divisions of the armies of the front over its line: while the front has fewer armies than sectors (the provinces
		// of the line, or the enemy provinces to attack), a division of the biggest idle army leaves it as an army of its own
		void spread_front_divisions(InstanceManager& instance_manager, Front& front, std::span<ProvinceInstance* const> zone = {}, bool limited = false);

		War* find_war(std::uint32_t id);

		void update_combat(InstanceManager& instance_manager);
		void update_sieges(InstanceManager& instance_manager);
		void update_scores(InstanceManager& instance_manager);
		void update_army_recovery(InstanceManager& instance_manager);
		void update_attrition(InstanceManager& instance_manager);
		void update_naval_combat(InstanceManager& instance_manager);
		void update_blockades(InstanceManager& instance_manager);
		void update_leaders(InstanceManager& instance_manager);

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

		War* find_war_between(CountryInstance const& first, CountryInstance const& second);
		// The war ends without a peace treaty (the countries are at peace again; the tests, the end of a country)
		void end_war(InstanceManager& instance_manager, std::uint32_t war_id);

		std::span<const Front> get_fronts() const {
			return fronts;
		}
		Front const* get_front_by_id(std::uint32_t id) const;
		bool is_auto_plan_enabled(CountryInstance const& country) const {
			return auto_plan_countries.contains(&country);
		}

		// Creates a front for the country. With an enemy the line is the border of the country with the land the enemy holds (auto front), without one
		// the front starts empty. Returns the id of the front, 0 on failure.
		std::uint32_t create_front(InstanceManager& instance_manager, CountryInstance& country, CountryInstance const* enemy);
		// A field marshal has one order (docs/WAR_ASSUMPTIONS.md, section 6): a front (along a border or a coast, with at most one offensive from it), a
		// line of defence or a naval invasion; a new front, line of defence or naval invasion deletes all his earlier lines, a new offensive the earlier one.
		// The front of a field marshal drawn on the map (0: nobody's): along the whole border with the neighbour (and, at war, with his allies in the
		// war with the country), the neighbour has to touch the land the country controls. Returns the id of the front, 0 on failure
		// With from and to (provinces of either side of the border) only the part of the border between them (in the order of our provinces along it)
		std::uint32_t create_border_front(
			InstanceManager& instance_manager, CountryInstance& country, std::uint32_t group_id, CountryInstance const& neighbour,
			ProvinceInstance const* from = nullptr, ProvinceInstance const* to = nullptr
		);
		// The pieces of the border of the country with the sector of the neighbour, all of them or the part between the two provinces
		memory::vector<FrontEdge> get_border_part(
			InstanceManager& instance_manager, CountryInstance const& country, CountryInstance const& neighbour, ProvinceInstance const* from,
			ProvinceInstance const* to
		);
		// A coastal front of a field marshal: the coast of the country next to the sea province. Returns the id of the front, 0 on failure
		std::uint32_t create_coastal_front(InstanceManager& instance_manager, CountryInstance& country, std::uint32_t group_id, ProvinceInstance const& sea);
		// The edit mode: a piece of the border between the provinces is added to the line of the front (or taken away if it is on it). The other province
		// has to be held by a country of the sector (a border front) or be the sea of a coastal front
		bool toggle_front_edge(InstanceManager& instance_manager, CountryInstance const& country, std::uint32_t front_id, ProvinceInstance& own, ProvinceInstance& other);
		bool delete_front(CountryInstance const& country, std::uint32_t front_id);
		// The lines of the plans of the field marshals. A new line starts with one province: an offensive with a target (land the country does not control,
		// its front is the front of the marshal nearest to the target), a defence with a province the country controls, a naval invasion with a port of the
		// country (its coast is added afterwards). Returns the id of the line, 0 on failure
		std::uint32_t create_plan_line(
			InstanceManager& instance_manager, CountryInstance& country, std::uint32_t group_id, PlanLine::type_t type, ProvinceInstance& province
		);
		// Adds the province to the line or takes it away (the last one cannot be taken: the line is deleted instead)
		bool toggle_plan_line_province(InstanceManager& instance_manager, CountryInstance const& country, std::uint32_t line_id, ProvinceInstance& province);
		bool delete_plan_line(CountryInstance const& country, std::uint32_t line_id);
		// A line of defence is a chain of neighbouring provinces (P9 of docs/WAR_ASSUMPTIONS.md, section 6): its end (the last province, or the first one)
		// goes to the province over the shortest way over our land (the gap is filled; no way from that end: from the other one); a province of the line
		// cuts it there
		bool extend_plan_line(InstanceManager& instance_manager, CountryInstance const& country, std::uint32_t line_id, bool at_end, ProvinceInstance& province);
		// The shortest way over our land (not over the chain) from an end of the chain to the province is added to it; false if there is none
		bool extend_chain(
			InstanceManager& instance_manager, CountryInstance const& country, memory::vector<ProvinceInstance*>& chain, bool at_end, ProvinceInstance& province
		) const;
		// The fronts and the lines of the plan of the field marshal are deleted (the armies stop where they are)
		bool delete_group_lines(InstanceManager& instance_manager, CountryInstance const& country, std::uint32_t group_id);
		std::span<const PlanLine> get_plan_lines() const {
			return plan_lines;
		}
		PlanLine const* get_plan_line_by_id(std::uint32_t id) const;
		// The enemy provinces of an offensive: its targets and the provinces between the front and them (docs/WAR_ASSUMPTIONS.md, section 6), in the order
		// of the province indices
		[[nodiscard]] memory::vector<ProvinceInstance*> get_offensive_zone(InstanceManager& instance_manager, PlanLine const& line) const;
		bool set_front_mode(CountryInstance const& country, std::uint32_t front_id, Front::mode_t mode);
		// The first front of the group (nullptr if it has none)
		Front const* find_front_of_group(CountryInstance const& country, std::uint32_t group_id) const;
		bool set_front_army(
			InstanceManager& instance_manager, CountryInstance const& country, std::uint32_t front_id, unique_id_t army_id,
			bool assigned
		);
		// Assigns every army of the country that is not on a front yet to the front
		bool assign_free_armies(InstanceManager& instance_manager, CountryInstance const& country, std::uint32_t front_id);
		void set_auto_plan(CountryInstance const& country, bool enabled);
		// The group of the front (0 takes it from the group); the front has to be the country's
		bool set_front_group(CountryInstance const& country, std::uint32_t front_id, std::uint32_t group_id);
		// Does the front get orders: a front without a group always does, a front of a group only while the group executes its plan
		[[nodiscard]] bool is_front_commanded(Front const& front) const;
		// The armies of the front stop where they are (the moves are cancelled, the armies in a battle stay in it)
		void stop_front_armies(InstanceManager& instance_manager, std::uint32_t front_id);

		void tick(InstanceManager& instance_manager);
	};
}

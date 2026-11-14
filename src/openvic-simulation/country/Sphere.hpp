#pragma once

#include <cstdint>
#include <string_view>

#include "openvic-simulation/core/object/Colour.hpp"
#include "openvic-simulation/core/object/Date.hpp"
#include "openvic-simulation/core/object/FixedPoint.hpp"
#include "openvic-simulation/core/memory/String.hpp"
#include "openvic-simulation/core/memory/Vector.hpp"
#include "openvic-simulation/country/SphereKind.hpp"
#include "openvic-simulation/dataloader/NodeTools.hpp"
#include "openvic-simulation/diplomacy/CountryRelation.hpp"

namespace OpenVic {
	struct CountryInstance;
	struct InstanceManager;

	// The spheres of influence (version 92, docs/PLAN_SPHERES.md, stage S1 and S2): a great power leads, the members join by the influence of the
	// leader on them (the dependence of their economy: the trade from the leader and its investments, times the relation and the neighbourhood)
	// (the kinds: SphereKind.hpp; their effects, version 93: the customs union, the imperial sphere, the military and the ideological union)

	struct Sphere {
		std::uint32_t id = 0;
		CountryInstance* leader = nullptr;
		sphere_kind_t kind = sphere_kind_t::CUSTOMS_UNION;
		colour_t colour {};
		std::uint32_t icon = 0;
		Date founded;
		// The leader is not a great power since this date (Date {}: it is one)
		Date leader_lost_rank;
		memory::vector<CountryInstance*> members; // without the leader
		// A sphere of the history (start_spheres.txt) lives to this date even when its leader is no great power (Date {}: not protected)
		Date protected_until;
		// The founding or the last change of the kind (the kind changes SPHERE_KIND_CHANGE_MONTHS after it)
		Date kind_changed;
		// The tribute of the imperial sphere the leader got yesterday (for the window of the sphere)
		fixed_point_t tribute_yesterday;
	};

	// The spheres at the start of the game, as in the history (ov_data/common/ov_spheres/start_spheres.txt; a mod replaces a leader's entry)
	struct StartSphereDefinitions {
		struct entry_t {
			memory::string leader;
			sphere_kind_t kind = sphere_kind_t::CUSTOMS_UNION;
			memory::vector<memory::string> members;
			Date from; // the sphere is set up when the game starts in [from, until)
			Date until;
		};

	private:
		memory::vector<entry_t> entries;

	public:
		memory::vector<entry_t> const& get_entries() const {
			return entries;
		}
		// The number of the entries read
		std::size_t load_file(std::string_view source, ast::NodeCPtr root);
	};

	struct SphereManager {
		// The influence of a great power on a country: its parts and the value (smoothed month by month)
		struct influence_t {
			fixed_point_t trade; // the points of the trade: 1 % of the GDP bought from the leader = 10
			fixed_point_t investment; // the points of the investments (the credits of the leader): 1 % of the GDP = 10
			fixed_point_t multiplier; // the sum of the percents of the relation and the neighbourhood (-1 .. +0.75)
			fixed_point_t value; // (trade + investment) x (1 + multiplier), smoothed
		};

	private:
		memory::vector<Sphere> spheres;
		std::uint32_t next_id = 1;
		vector_ordered_map<CountryInstancePair, influence_t> influence; // (great power, country)
		// The members of the history: the AI does not leave and is not drawn away before this date
		vector_ordered_map<CountryInstance const*, Date> protected_until;


		void set_membership(CountryInstance& country, CountryInstance const* leader, sphere_kind_t kind);
		void dissolve(InstanceManager& instance_manager, std::size_t index);
		void update_influence(InstanceManager& instance_manager);

	public:
		memory::vector<Sphere> const& get_spheres() const {
			return spheres;
		}
		Sphere const* find_led_by(CountryInstance const* leader) const;
		// A member of a sphere of the history that the AI keeps in it (SPHERE_START_PROTECTION_YEARS)
		bool is_protected(CountryInstance const* country, Date today) const;
		Sphere const* find_of(CountryInstance const* country) const; // led by it or with it as a member
		influence_t get_influence(CountryInstance const* leader, CountryInstance const* country) const;
		decltype(influence)::values_container_type const& get_influence_values() const {
			return influence.values_container();
		}

		// "" if the country may found a sphere of this kind, else the reason
		std::string_view found_block_reason(InstanceManager const& instance_manager, CountryInstance const& country, sphere_kind_t kind) const;
		bool found(InstanceManager& instance_manager, CountryInstance& country, sphere_kind_t kind);
		// "" if the leader may invite the country, else the reason
		std::string_view invite_block_reason(InstanceManager const& instance_manager, CountryInstance const& leader, CountryInstance const& country) const;
		// Whether the country (an AI) accepts to join the sphere of the leader
		bool accepts(InstanceManager const& instance_manager, CountryInstance const& leader, CountryInstance const& country) const;
		bool invite(InstanceManager& instance_manager, CountryInstance& leader, CountryInstance& country);
		bool leave(InstanceManager& instance_manager, CountryInstance& country);
		bool set_look(CountryInstance const& leader, colour_t colour, std::uint32_t icon);
		// "" if the leader may change the kind of its sphere to this one, else the reason (version 93)
		std::string_view change_kind_block_reason(InstanceManager const& instance_manager, CountryInstance const& leader, sphere_kind_t kind) const;
		bool change_kind(InstanceManager& instance_manager, CountryInstance& leader, sphere_kind_t kind);
		// The date the leader may change the kind again
		Date get_kind_change_date(InstanceManager const& instance_manager, Sphere const& sphere) const;

		// At the start of the game: the spheres of the history (start_spheres.txt), the leaders need not be great powers yet
		void setup_start_spheres(InstanceManager& instance_manager, StartSphereDefinitions const& definitions);

		// Once a month: the influence, the end of the spheres whose leader is no great power any more, the decisions of the AI
		void monthly_update(InstanceManager& instance_manager);
		// Every day: the tribute of the members of the imperial spheres to their leaders (version 93)
		void daily_update(InstanceManager& instance_manager);
	};
}

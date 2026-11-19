#include "Subjects.hpp"

#include <algorithm>

#include "openvic-simulation/DefinitionManager.hpp"
#include "openvic-simulation/InstanceManager.hpp"
#include "openvic-simulation/country/CountryInstance.hpp"
#include "openvic-simulation/defines/OpenVicDefines.hpp"
#include "openvic-simulation/diplomacy/CountryRelation.hpp"
#include "openvic-simulation/history/DiplomaticHistory.hpp"
#include "openvic-simulation/map/MapInstance.hpp"
#include "openvic-simulation/map/ProvinceInstance.hpp"
#include "openvic-simulation/map/State.hpp"
#include "openvic-simulation/country/Sphere.hpp"
#include "openvic-simulation/economy/GoodInstance.hpp"
#include "openvic-simulation/defines/DiplomacyDefines.hpp"
#include "openvic-simulation/diplomacy/DiplomaticAction.hpp"
#include "openvic-simulation/military/UnitInstanceGroup.hpp"
#include "openvic-simulation/misc/NotificationLog.hpp"
#include "openvic-simulation/utility/Logger.hpp"

using namespace OpenVic;
using subject_type_t = CountryInstance::subject_type_t;

namespace {
	OpenVicDiplomacyDefines const& defines_of(InstanceManager const& instance_manager) {
		return instance_manager.definition_manager.get_define_manager().get_openvic_defines().diplomacy;
	}
}

std::string_view Subjects::get_type_key(const subject_type_t type) {
	switch (type) {
	case subject_type_t::PUPPET: return "puppet";
	case subject_type_t::PROTECTORATE: return "protectorate";
	case subject_type_t::COLONIAL: return "colonial";
	case subject_type_t::CROWN_LAND: return "crown_land";
	case subject_type_t::DOMINION: return "dominion";
	default: return "";
	}
}

bool Subjects::overlord_defends(const subject_type_t type) {
	return type == subject_type_t::PUPPET || type == subject_type_t::PROTECTORATE || type == subject_type_t::COLONIAL
		|| type == subject_type_t::CROWN_LAND;
}

bool Subjects::overlord_joins_attacks(const subject_type_t type) {
	return type == subject_type_t::COLONIAL || type == subject_type_t::CROWN_LAND;
}

bool Subjects::may_attack_overlord(const subject_type_t type) {
	return type == subject_type_t::PUPPET || type == subject_type_t::DOMINION;
}

bool Subjects::has_foreign_policy(const subject_type_t type) {
	return type != subject_type_t::PUPPET;
}

bool Subjects::tribute_is_changeable(const subject_type_t type) {
	return type == subject_type_t::COLONIAL || type == subject_type_t::CROWN_LAND;
}

std::int32_t Subjects::get_relation_bonus(OpenVicDiplomacyDefines const& defines, const subject_type_t type) {
	switch (type) {
	case subject_type_t::PUPPET: return defines.subject_relation_puppet;
	case subject_type_t::PROTECTORATE: return defines.subject_relation_protectorate;
	case subject_type_t::COLONIAL: return defines.subject_relation_colonial;
	case subject_type_t::CROWN_LAND: return defines.subject_relation_crown_land;
	case subject_type_t::DOMINION: return defines.subject_relation_dominion;
	default: return 0;
	}
}

std::int32_t Subjects::get_default_tribute_pct(OpenVicDiplomacyDefines const& defines, const subject_type_t type) {
	switch (type) {
	case subject_type_t::PUPPET: return defines.subject_tribute_puppet_pct;
	case subject_type_t::PROTECTORATE: return defines.subject_tribute_protectorate_pct;
	case subject_type_t::COLONIAL: return defines.subject_tribute_colonial_pct;
	case subject_type_t::CROWN_LAND: return defines.subject_tribute_crown_land_pct;
	case subject_type_t::DOMINION: return defines.subject_tribute_dominion_pct;
	default: return 0;
	}
}

std::int32_t Subjects::get_min_tribute_pct(OpenVicDiplomacyDefines const& defines, const subject_type_t type) {
	if (type == subject_type_t::COLONIAL) {
		return defines.subject_colonial_min_pct;
	}
	if (type == subject_type_t::CROWN_LAND) {
		return defines.subject_crown_land_min_pct;
	}
	return get_default_tribute_pct(defines, type);
}

std::int32_t Subjects::get_max_tribute_pct(OpenVicDiplomacyDefines const& defines, const subject_type_t type) {
	if (type == subject_type_t::COLONIAL) {
		return defines.subject_colonial_max_pct;
	}
	if (type == subject_type_t::CROWN_LAND) {
		return defines.subject_crown_land_max_pct;
	}
	return get_default_tribute_pct(defines, type);
}

subject_type_t Subjects::get_more_autonomy(const subject_type_t type) {
	switch (type) {
	case subject_type_t::PUPPET: return subject_type_t::PROTECTORATE;
	case subject_type_t::PROTECTORATE: return subject_type_t::COLONIAL;
	case subject_type_t::COLONIAL: return subject_type_t::CROWN_LAND;
	case subject_type_t::CROWN_LAND: return subject_type_t::DOMINION;
	default: return subject_type_t::NONE;
	}
}

void Subjects::set_type(InstanceManager& instance_manager, CountryInstance& subject, const subject_type_t type) {
	OpenVicDiplomacyDefines const& defines = defines_of(instance_manager);
	subject.subject_type = type;
	subject.subject_tribute_pct = get_default_tribute_pct(defines, type);
	subject.subject_goods_share.clear();
	// a colonial subject: more raw goods, fewer factory goods
	if (type == subject_type_t::COLONIAL) {
		subject.subject_rgo_multiplier = fixed_point_t::_1 + fixed_point_t(defines.subject_colonial_rgo_pct) / 100;
		subject.subject_factory_multiplier = fixed_point_t::_1 - fixed_point_t(defines.subject_colonial_factory_pct) / 100;
	} else {
		subject.subject_rgo_multiplier = fixed_point_t::_1;
		subject.subject_factory_multiplier = fixed_point_t::_1;
	}
	// a puppet has no foreign policy: it leaves its alliances
	if (!has_foreign_policy(type)) {
		CountryRelationManager& relations = instance_manager.get_country_relation_manager();
		for (CountryInstance& other : instance_manager.get_country_instance_manager().get_country_instances()) {
			if (&other != &subject && subject.has_alliance_with(other)) {
				relations.set_country_alliance(&subject, &other, false);
			}
		}
		subject.justify_target = nullptr;
		subject.justify_days_left = 0;
	}
}

void Subjects::make_subject(
	InstanceManager& instance_manager, CountryInstance& subject, CountryInstance& overlord, const subject_type_t type, const bool notify
) {
	if (&subject == &overlord || type == subject_type_t::NONE) {
		return;
	}
	// a subject has no subjects of its own, and an overlord is nobody's subject
	for (CountryInstance& other : instance_manager.get_country_instance_manager().get_country_instances()) {
		if (other.overlord == &subject) {
			release(instance_manager, other, notify);
		}
	}
	if (overlord.overlord == &subject) {
		release(instance_manager, overlord, notify);
	}
	subject.overlord = &overlord;
	subject.subject_since = instance_manager.get_today();
	set_type(instance_manager, subject, type);
	SPDLOG_INFO("Subjects: {} becomes a {} of {}", subject.get_identifier(), get_type_key(type), overlord.get_identifier());
	if (notify) {
		instance_manager.get_notification_log().add(instance_manager.get_today(), &subject, "became_subject", get_type_key(type), &overlord);
	}
}

void Subjects::release(InstanceManager& instance_manager, CountryInstance& subject, const bool notify) {
	CountryInstance* const overlord = subject.overlord;
	if (overlord == nullptr) {
		return;
	}
	SPDLOG_INFO("Subjects: {} is free of {}", subject.get_identifier(), overlord->get_identifier());
	subject.overlord = nullptr;
	subject.subject_type = subject_type_t::NONE;
	subject.subject_tribute_pct = 0;
	subject.subject_rgo_multiplier = fixed_point_t::_1;
	subject.subject_factory_multiplier = fixed_point_t::_1;
	subject.subject_tribute_yesterday = fixed_point_t::_0;
	subject.subject_goods_share.clear();
	if (notify && subject.exists() && overlord->exists()) {
		instance_manager.get_notification_log().add(instance_manager.get_today(), &subject, "subject_released", "", overlord);
	}
}

memory::string Subjects::get_annex_block_reason(
	InstanceManager const& instance_manager, CountryInstance const& overlord, CountryInstance const& subject
) {
	if (subject.overlord != &overlord) {
		return "Это не наш субъект";
	}
	if (overlord.is_at_war() || subject.is_at_war()) {
		return "Нельзя во время войны";
	}
	OpenVicDiplomacyDefines const& defines = defines_of(instance_manager);
	const std::int32_t days = (instance_manager.get_today() - subject.subject_since).to_int();
	if (days < defines.subject_annex_min_years * 365) {
		return memory::fmt::format("Субъект должен пробыть под нами {} лет (осталось {} дн.)", defines.subject_annex_min_years,
			defines.subject_annex_min_years * 365 - days);
	}
	const std::int32_t relation = instance_manager.get_country_relation_manager().get_effective_relation(&overlord, &subject);
	if (relation < defines.subject_annex_min_relation) {
		return memory::fmt::format("Нужны отношения не ниже {} (сейчас {})", defines.subject_annex_min_relation, relation);
	}
	return {};
}

bool Subjects::annex(InstanceManager& instance_manager, CountryInstance& overlord, CountryInstance& subject) {
	if (!get_annex_block_reason(instance_manager, overlord, subject).empty()) {
		return false;
	}
	SPDLOG_INFO("Subjects: {} annexes {}", overlord.get_identifier(), subject.get_identifier());
	instance_manager.get_notification_log().add(instance_manager.get_today(), &overlord, "subject_annexed", "", &subject);
	absorb(instance_manager, overlord, subject);
	return true;
}

void Subjects::absorb(InstanceManager& instance_manager, CountryInstance& overlord, CountryInstance& subject) {
	// (it leaves its sphere first, version 103)
	if (subject.get_sphere_owner_untracked() != nullptr) {
		instance_manager.get_sphere_manager().leave(instance_manager, subject);
	}
	// the land (a copy: the owner changes)
	const memory::vector<ProvinceInstance*> provinces { subject.get_owned_provinces().begin(), subject.get_owned_provinces().end() };
	for (ProvinceInstance* province : provinces) {
		province->set_owner(&overlord);
		province->set_controller(&overlord);
	}
	// the armies and the fleets are disbanded (a copy: they are deleted)
	UnitInstanceManager& units = instance_manager.get_unit_instance_manager();
	memory::vector<UnitInstanceGroup*> groups;
	for (ArmyInstance& army : subject.get_armies()) {
		groups.push_back(&army);
	}
	for (NavyInstance& navy : subject.get_navies()) {
		groups.push_back(&navy);
	}
	for (UnitInstanceGroup* group : groups) {
		units.destroy_group(*group);
	}
	// its bonds with the others end
	CountryRelationManager& relations = instance_manager.get_country_relation_manager();
	for (CountryInstance& other : instance_manager.get_country_instance_manager().get_country_instances()) {
		if (&other == &subject) {
			continue;
		}
		if (subject.has_alliance_with(other)) {
			relations.set_country_alliance(&subject, &other, false);
		}
		if (subject.has_military_access_to(other)) {
			relations.set_has_military_access_to(&subject, &other, false);
		}
		if (other.has_military_access_to(subject)) {
			relations.set_has_military_access_to(&other, &subject, false);
		}
		if (subject.is_sending_war_subsidy_to(other)) {
			relations.set_war_subsidies_to(&subject, &other, false);
		}
		if (other.is_sending_war_subsidy_to(subject)) {
			relations.set_war_subsidies_to(&other, &subject, false);
		}
		if (other.overlord == &subject) {
			release(instance_manager, other, false);
		}
	}
	subject.set_capital(nullptr);
	subject.justify_target = nullptr;
	subject.justify_days_left = 0;
	subject.casus_belli.clear();
	release(instance_manager, subject, false);
}

void Subjects::setup_history(InstanceManager& instance_manager) {
	CountryInstanceManager& countries = instance_manager.get_country_instance_manager();
	const Date today = instance_manager.get_today();
	DiplomaticHistoryManager const& history = instance_manager.definition_manager.get_history_manager().get_diplomacy_manager();
	for (SubjectHistory const& entry : history.get_subjects(today)) {
		CountryInstance* const overlord = &countries.get_country_instance_by_definition(entry.overlord);
		CountryInstance* const subject = &countries.get_country_instance_by_definition(entry.subject);
		if (overlord == subject || subject->overlord != nullptr || overlord->overlord != nullptr
			|| !subject->exists() || !overlord->exists()) {
			continue;
		}
		const bool has_subjects = std::any_of(countries.get_country_instances().begin(), countries.get_country_instances().end(),
			[subject](CountryInstance const& other) -> bool { return other.overlord == subject; });
		if (has_subjects) {
			continue;
		}
		make_subject(instance_manager, *subject, *overlord,
			entry.subject_type == SubjectHistory::type_t::SUBSTATE ? subject_type_t::CROWN_LAND : subject_type_t::PUPPET, false);
	}
}

void Subjects::daily_update(InstanceManager& instance_manager) {
	for (CountryInstance& subject : instance_manager.get_country_instance_manager().get_country_instances()) {
		CountryInstance* const overlord = subject.overlord;
		if (overlord == nullptr) {
			continue;
		}
		subject.subject_tribute_yesterday = fixed_point_t::_0;
		if (!subject.exists() || !overlord->exists()) {
			release(instance_manager, subject, false);
			continue;
		}
		// no payments in the war of independence
		if (subject.is_at_war_with(*overlord)) {
			subject.subject_goods_share.clear();
			continue;
		}
		// the raw goods of a colonial subject (version 102): what its RGOs put aside today goes to the national stockpile of the overlord, and
		// the shares of tomorrow are its surplus of every good today
		if (subject.subject_type == subject_type_t::COLONIAL) {
			for (ProvinceInstance* province : subject.get_owned_provinces()) {
				ResourceGatheringOperation const& rgo = province->get_rgo();
				if (rgo.to_overlord_today > fixed_point_t::_0 && rgo.get_production_type_nullable() != nullptr) {
					overlord->get_good_data(rgo.get_production_type_nullable()->output_good).stockpile_amount += rgo.to_overlord_today;
				}
			}
			const std::int32_t pct = defines_of(instance_manager).subject_colonial_raw_surplus_pct;
			auto goods = instance_manager.get_good_instance_manager().get_good_instances();
			subject.subject_goods_share.assign(goods.size(), fixed_point_t::_0);
			for (GoodInstance const& good : goods) {
				if (good.good_definition.is_money || pct <= 0) {
					continue;
				}
				CountryInstance::good_data_t const& data = subject.get_good_data(good);
				fixed_point_t produced = fixed_point_t::_0;
				for (auto const& [production_type, quantity] : data.production_per_production_type) {
					produced += quantity;
				}
				const fixed_point_t demand = data.pop_demand + data.factory_demand + data.army_needs + data.navy_needs;
				if (produced > demand && produced > fixed_point_t::_0) {
					subject.subject_goods_share[static_cast<std::size_t>(type_safe::get(good.index))] = (produced - demand) / produced * pct / 100;
				}
			}
		}
		if (subject.subject_tribute_pct <= 0) {
			continue;
		}
		// a share of the income of yesterday; a colonial subject: a share of the growth of its treasury (the average balance, when it is positive)
		const fixed_point_t base = subject.subject_type == subject_type_t::COLONIAL ? subject.get_balance_before_investment_average()
			: subject.get_income_yesterday();
		if (base <= fixed_point_t::_0) {
			continue;
		}
		const fixed_point_t amount = std::min(base * subject.subject_tribute_pct / 100, subject.get_cash_stockpile().load());
		if (amount > fixed_point_t::_0) {
			subject.spend_cash(amount);
			overlord->add_cash(amount);
			subject.subject_tribute_yesterday = amount;
		}
	}
}

void Subjects::monthly_update(InstanceManager& instance_manager) {
	OpenVicDiplomacyDefines const& defines = defines_of(instance_manager);
	if (defines.subject_discontent_per_month <= 0) {
		return;
	}
	CountryRelationManager& relations = instance_manager.get_country_relation_manager();
	for (CountryInstance& subject : instance_manager.get_country_instance_manager().get_country_instances()) {
		if (subject.overlord == nullptr || !subject.exists()) {
			continue;
		}
		// everybody wants to be sovereign: the relation goes down to SUBJECT_DISCONTENT_FLOOR
		const std::int32_t relation = relations.get_country_relation(&subject, subject.overlord);
		if (relation > defines.subject_discontent_floor) {
			relations.change_relation(&subject, subject.overlord, -std::min(defines.subject_discontent_per_month, relation - defines.subject_discontent_floor));
		}
	}
}

memory::vector<Subjects::releasable_t> Subjects::get_releasable_nations(CountryInstance const& country) {
	memory::vector<releasable_t> result;
	if (!country.exists()) {
		return result;
	}
	State const* const capital_state = country.get_capital() != nullptr ? country.get_capital()->get_state() : nullptr;
	for (State* state : country.get_states()) {
		if (state == nullptr || state == capital_state || state->get_capital() == nullptr) {
			continue;
		}
		ProvinceInstance const& state_capital = *state->get_capital();
		bool colonial = false;
		std::size_t provinces = 0;
		for (ProvinceInstance const& province : state->get_provinces()) {
			colonial = colonial || province.get_colony_status() != colony_status_t::STATE;
			provinces++;
		}
		// the nations whose core is the capital of the state (in the order of the cores)
		for (CountryInstance* nation : state_capital.get_cores()) {
			// (not a union that was formed, version 103)
			if (nation == nullptr || nation == &country || nation->exists() || nation->union_holder != nullptr
				|| &nation->country_definition == country.union_identity || nation->is_rebel_country() || !nation->is_releasable_vassal()
				|| nation->country_definition.is_dynamic_tag) {
				continue;
			}
			auto it = std::find_if(result.begin(), result.end(), [nation](releasable_t const& entry) -> bool { return entry.nation == nation; });
			if (it == result.end()) {
				result.push_back({ nation });
				it = result.end() - 1;
			}
			it->states.push_back(state);
			it->provinces += provinces;
			it->population += static_cast<std::int64_t>(type_safe::get(state->get_total_population()));
			it->colonial = it->colonial || colonial;
		}
	}
	std::sort(result.begin(), result.end(), [](releasable_t const& a, releasable_t const& b) -> bool {
		return type_safe::get(a.nation->index) < type_safe::get(b.nation->index);
	});
	return result;
}

std::string_view Subjects::get_release_block_reason(CountryInstance const& country, releasable_t const& entry, const subject_type_t type) {
	if (entry.nation == nullptr || entry.states.empty()) {
		return "Нечего освобождать";
	}
	if (country.is_at_war()) {
		return "Нельзя во время войны";
	}
	if (type != subject_type_t::NONE && country.overlord != nullptr) {
		return "Субъект освобождает нации только независимыми";
	}
	if (type == subject_type_t::COLONIAL && !entry.colonial) {
		return "Колониальный субъект — только из колониальных земель";
	}
	return {};
}

bool Subjects::release_nation(InstanceManager& instance_manager, CountryInstance& country, CountryInstance& nation, const subject_type_t type) {
	memory::vector<releasable_t> const releasable = get_releasable_nations(country);
	const auto it = std::find_if(releasable.begin(), releasable.end(), [&nation](releasable_t const& entry) -> bool { return entry.nation == &nation; });
	if (it == releasable.end() || !get_release_block_reason(country, *it, type).empty()) {
		return false;
	}
	OpenVicDiplomacyDefines const& defines = defines_of(instance_manager);
	// the share of the treasury of the population that leaves
	std::int64_t total_population = 0;
	for (State const* state : country.get_states()) {
		if (state != nullptr) {
			total_population += static_cast<std::int64_t>(type_safe::get(state->get_total_population()));
		}
	}
	fixed_point_t cash = fixed_point_t::_0;
	if (total_population > 0 && it->population > 0) {
		const std::int32_t permille = static_cast<std::int32_t>(std::min<std::int64_t>(1000, it->population * 1000 / total_population));
		cash = std::max(country.get_cash_stockpile().load(), fixed_point_t::_0) * permille / 1000;
	}
	SPDLOG_INFO("Subjects: {} releases {} ({} states){}", country.get_identifier(), nation.get_identifier(), it->states.size(),
		type != subject_type_t::NONE ? " as a subject" : "");
	// the land, whole states
	for (State* state : it->states) {
		for (ProvinceInstance& province : state->get_provinces()) {
			province.set_owner(&nation);
			province.set_controller(&nation);
		}
	}
	ProvinceInstance* const capital = nation.get_capital();
	if (capital == nullptr || capital->get_owner() != &nation) {
		nation.set_capital(nation.get_owned_provinces().empty() ? nullptr : *nation.get_owned_provinces().begin());
	}
	if (cash > fixed_point_t::_0) {
		country.spend_cash(cash);
		nation.add_cash(cash);
	}
	nation.set_diplomatic_points(defines.diplomatic_points_start);
	nation.casus_belli.clear();
	nation.justify_target = nullptr;
	nation.justify_days_left = 0;
	DiplomacyDefines const& vic2 = instance_manager.definition_manager.get_define_manager().get_diplomacy_defines();
	country.add_prestige(vic2.get_release_nation_prestige());
	country.change_infamy(vic2.get_release_nation_infamy());
	instance_manager.get_country_relation_manager().set_country_relation(&country, &nation, defines.release_nation_relation);
	if (type != subject_type_t::NONE) {
		make_subject(instance_manager, nation, country, type, false);
	} else {
		// the armies of the country leave the land of a free nation
		DiplomaticActionManager::expel_armies(instance_manager, country, nation);
	}
	instance_manager.get_notification_log().add(instance_manager.get_today(), &country, "nation_released", get_type_key(type), &nation);
	return true;
}

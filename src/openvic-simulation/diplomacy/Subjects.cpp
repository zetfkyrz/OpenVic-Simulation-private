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
	instance_manager.get_notification_log().add(instance_manager.get_today(), &overlord, "subject_annexed", "", &subject);
	release(instance_manager, subject, false);
	return true;
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
		if (subject.is_at_war_with(*overlord) || subject.subject_tribute_pct <= 0) {
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

#pragma once

#include <cstdint>
#include <string_view>

#include "openvic-simulation/core/memory/String.hpp"
#include "openvic-simulation/core/memory/Vector.hpp"
#include "openvic-simulation/core/object/Date.hpp"

namespace OpenVic {
	struct CountryInstance;

	/* Events of the game that the interface shows to the player (an invention was discovered, an election was won, a
	 * rising started, a war began or ended, ...). The simulation stores only what happened, as a kind and identifiers; the
	 * interface makes the text. The log keeps the last events. */
	struct NotificationLog {
		struct Entry {
			std::uint64_t id;
			Date date;
			CountryInstance const* country; // whom it concerns
			CountryInstance const* other_country; // the second country of a war or peace, may be nullptr
			memory::string kind; // "invention", "technology", "election", "uprising", "war_declared", "peace_signed", ...
			memory::string subject; // identifier of the invention / party / province ...
		};

	private:
		memory::vector<Entry> entries;
		std::uint64_t next_id = 1;
		static constexpr std::size_t MAX_ENTRIES = 300;

	public:
		void add(
			Date date, CountryInstance const* country, std::string_view kind, std::string_view subject = {},
			CountryInstance const* other_country = nullptr
		) {
			if (entries.size() >= MAX_ENTRIES) {
				entries.erase(entries.begin(), entries.begin() + MAX_ENTRIES / 3);
			}
			entries.push_back({ next_id++, date, country, other_country, memory::string { kind }, memory::string { subject } });
		}
		memory::vector<Entry> const& get_entries() const {
			return entries;
		}
	};
}

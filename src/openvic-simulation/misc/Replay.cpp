#include "Replay.hpp"

#include <sstream>
#include <utility>
#include <tuple>
#include <type_traits>

namespace OpenVic {
	namespace {
		template<typename T> struct is_pair : std::false_type {};
		template<typename A, typename B> struct is_pair<std::pair<A, B>> : std::true_type {};
		template<typename T> struct is_tuple : std::false_type {};
		template<typename... A> struct is_tuple<std::tuple<A...>> : std::true_type {};

		template<typename T>
		void write_value(std::ostream& os, T const& value) {
			if constexpr (std::is_same_v<T, fixed_point_t>) {
				os << ' ' << value.get_raw_value();
			} else if constexpr (std::is_same_v<T, bool>) {
				os << ' ' << (value ? 1 : 0);
			} else if constexpr (std::is_same_v<T, std::monostate>) {
			} else if constexpr (std::is_enum_v<T>) {
				os << ' ' << static_cast<std::int64_t>(value);
			} else if constexpr (std::is_integral_v<T>) {
				os << ' ' << value;
			} else if constexpr (is_pair<T>::value) {
				write_value(os, value.first);
				write_value(os, value.second);
			} else if constexpr (is_tuple<T>::value) {
				std::apply([&os](auto const&... elements) { (write_value(os, elements), ...); }, value);
			} else {
				write_value(os, type_safe::get(value));
			}
		}

		template<typename T>
		bool read_value(std::istream& is, T& value) {
			if constexpr (std::is_same_v<T, fixed_point_t>) {
				std::int64_t raw;
				if (!(is >> raw)) {
					return false;
				}
				value = fixed_point_t::parse_raw(raw);
				return true;
			} else if constexpr (std::is_same_v<T, bool>) {
				int raw;
				if (!(is >> raw)) {
					return false;
				}
				value = raw != 0;
				return true;
			} else if constexpr (std::is_same_v<T, std::monostate>) {
				return true;
			} else if constexpr (std::is_enum_v<T>) {
				std::int64_t raw;
				if (!(is >> raw)) {
					return false;
				}
				value = static_cast<T>(raw);
				return true;
			} else if constexpr (std::is_integral_v<T>) {
				return static_cast<bool>(is >> value);
			} else if constexpr (is_pair<T>::value) {
				return read_value(is, value.first) && read_value(is, value.second);
			} else if constexpr (is_tuple<T>::value) {
				return std::apply([&is](auto&... elements) { return (read_value(is, elements) && ...); }, value);
			} else {
				using under = std::remove_cv_t<std::remove_reference_t<decltype(type_safe::get(value))>>;
				under raw {};
				if (!read_value(is, raw)) {
					return false;
				}
				value = T(raw);
				return true;
			}
		}
	}

	bool Replay::is_recorded(game_action_t const& action) {
		return !std::holds_alternative<none_argument_t>(action) && !std::holds_alternative<set_pause_argument_t>(action) &&
			!std::holds_alternative<set_speed_argument_t>(action);
	}

	std::string Replay::serialise(std::vector<ReplayEntry> const& entries) {
		std::ostringstream os;
		os << "OVREPLAY 1\n";
		for (ReplayEntry const& entry : entries) {
			std::visit(
				[&](auto const& argument) {
					using arg_t = std::decay_t<decltype(argument)>;
					if constexpr (!std::is_same_v<arg_t, none_argument_t>) {
						os << entry.batch << ' ' << arg_t::action_name;
						write_value(os, type_safe::get(argument));
						os << '\n';
					}
				},
				entry.action
			);
		}
		return os.str();
	}

	bool Replay::deserialise(std::string const& text, std::vector<ReplayEntry>& entries) {
		std::istringstream stream(text);
		std::string line;
		if (!std::getline(stream, line) || line.rfind("OVREPLAY", 0) != 0) {
			return false;
		}
		while (std::getline(stream, line)) {
			if (line.empty()) {
				continue;
			}
			std::istringstream is(line);
			std::uint32_t batch;
			std::string name;
			if (!(is >> batch >> name)) {
				return false;
			}
			bool found = false;
#define READ_ACTION(action_name, ...) \
			if (!found && name == #action_name) { \
				found = true; \
				action_name##_argument_t::underlying_type value {}; \
				if (!read_value(is, value)) { \
					return false; \
				} \
				entries.push_back({ batch, game_action_t(std::in_place_type<action_name##_argument_t>, std::as_const(value)) }); \
			}
			FOR_EACH_GAME_ACTION(READ_ACTION)
#undef READ_ACTION
			if (!found) {
				return false;
			}
		}
		return true;
	}
}

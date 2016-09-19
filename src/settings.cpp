/***************************************************************************
 *   Copyright (C) 2008-2014 by Andrzej Rybczak                            *
 *   electricityispower@gmail.com                                          *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this program; if not, write to the                         *
 *   Free Software Foundation, Inc.,                                       *
 *   51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA.              *
 ***************************************************************************/

#include <boost/tuple/tuple.hpp>
#include <fstream>
#include <stdexcept>

#include "configuration.h"
#include "format_impl.h"
#include "helpers.h"
#include "settings.h"
#include "utility/conversion.h"
#include "utility/option_parser.h"
#include "utility/type_conversions.h"

#ifdef HAVE_LANGINFO_H
# include <langinfo.h>
#endif

Configuration Config;

namespace {

std::vector<Column> generate_columns(const std::string &format)
{
	std::vector<Column> result;
	std::string width;
	size_t pos = 0;
	while (!(width = getEnclosedString(format, '(', ')', &pos)).empty())
	{
		Column col;
		auto scolor = getEnclosedString(format, '[', ']', &pos);
		if (scolor.empty())
			col.color = NC::Color::Default;
		else
			col.color = boost::lexical_cast<NC::Color>(scolor);

		if (*width.rbegin() == 'f')
		{
			col.fixed = true;
			width.resize(width.size()-1);
		}
		else
			col.fixed = false;

		auto tag_type = getEnclosedString(format, '{', '}', &pos);
		// alternative name
		size_t tag_type_colon_pos = tag_type.find(':');
		if (tag_type_colon_pos != std::string::npos)
		{
			col.name = ToWString(tag_type.substr(tag_type_colon_pos+1));
			tag_type.resize(tag_type_colon_pos);
		}

		if (!tag_type.empty())
		{
			size_t i = -1;

			// extract tag types in format a|b|c etc.
			do
				col.type += tag_type[(++i)++]; // nice one.
			while (tag_type[i] == '|');

			// apply attributes
			for (; i < tag_type.length(); ++i)
			{
				switch (tag_type[i])
				{
					case 'r':
						col.right_alignment = 1;
						break;
					case 'E':
						col.display_empty_tag = 0;
						break;
				}
			}
		}
		else // empty column
			col.display_empty_tag = 0;

		col.width = boost::lexical_cast<int>(width);
		result.push_back(col);
	}

	// calculate which column is the last one to have relative width and stretch it accordingly
	if (!result.empty())
	{
		int stretch_limit = 0;
		auto it = result.rbegin();
		for (; it != result.rend(); ++it)
		{
			if (it->fixed)
				stretch_limit += it->width;
			else
				break;
		}
		// if it's false, all columns are fixed
		if (it != result.rend())
			it->stretch_limit = stretch_limit;
	}

	return result;
}

Format::AST<char> columns_to_format(const std::vector<Column> &columns)
{
	std::vector<Format::Expression<char>> result;

	auto column = columns.begin();
	while (true)
	{
		Format::FirstOf<char> first_of;
		for (const auto &type : column->type)
		{
			auto f = charToGetFunction(type);
			assert(f != nullptr);
			first_of.base().push_back(f);
		}
		result.push_back(std::move(first_of));

		if (++column != columns.end())
			result.push_back(" ");
		else
			break;
	}

	return Format::AST<char>(std::move(result));
}

void add_slash_at_the_end(std::string &s)
{
	if (s.empty() || *s.rbegin() != '/')
	{
		s.resize(s.size()+1);
		*s.rbegin() = '/';
	}
}

std::string adjust_directory(std::string s)
{
	add_slash_at_the_end(s);
	expand_home(s);
	return s;
}

// parser worker for buffer
template <typename ValueT, typename TransformT>
option_parser::worker buffer(NC::Buffer &arg, ValueT &&value, TransformT &&map)
{
	return option_parser::worker(assign<std::string>(arg, [&arg, map](std::string s) {
		NC::Buffer result;
		auto ast = Format::parse(s, Format::Flags::Color | Format::Flags::Format);
		Format::print(ast, result, nullptr);
		return map(std::move(result));
	}), defaults_to(arg, map(std::forward<ValueT>(value))));
}

option_parser::worker border(NC::Border &arg, NC::Border value)
{
	return option_parser::worker(assign<std::string>(arg, [&arg](std::string s) {
		NC::Border result;
		if (!s.empty())
		{
			try {
				result = boost::lexical_cast<NC::Color>(s);
			} catch (boost::bad_lexical_cast &) {
				throw std::runtime_error("invalid border: " + s);
			}
		}
		return result;
	}), defaults_to(arg, std::move(value)));
}

option_parser::worker deprecated(const char *option, double version_removal)
{
	return option_parser::worker([option, version_removal](std::string) {
		std::cerr << "WARNING: " << option << " is deprecated and will be removed in " << version_removal << ".\n";
	}, [] { }
	);
}

}

bool Configuration::read(const std::vector<std::string> &config_paths, bool ignore_errors)
{
	std::string mpd_host;
	unsigned mpd_port;
	std::string columns_format;

	option_parser p;

	// deprecation warnings
	p.add("default_space_mode", deprecated("default_space_mode", 0.8));

	// keep the same order of variables as in configuration file
	p.add("ncmpcpp_directory", assign_default<std::string>(
		ncmpcpp_directory, "~/.ncmpcpp/", adjust_directory
	));
	p.add("lyrics_directory", assign_default<std::string>(
		lyrics_directory, "~/.lyrics/", adjust_directory
	));
	p.add("mpd_host", assign_default<std::string>(
		mpd_host, "localhost", [](std::string host) {
			// host can be a path to ipc socket, relative to home directory
			expand_home(host);
			Mpd.SetHostname(host);
			return host;
	}));
	p.add("mpd_port", assign_default<unsigned>(
		mpd_port, 6600, [](unsigned port) {
			Mpd.SetPort(port);
			return port;
	}));
	p.add("mpd_music_dir", assign_default<std::string>(
		mpd_music_dir, "~/music", adjust_directory
	));
	p.add("mpd_connection_timeout", assign_default(
		mpd_connection_timeout, 5
	));
	p.add("mpd_crossfade_time", assign_default(
		crossfade_time, 5
	));
	p.add("visualizer_fifo_path", assign_default(
		visualizer_fifo_path, "/tmp/mpd.fifo"
	));
	p.add("visualizer_output_name", assign_default(
		visualizer_output_name, "Visualizer feed"
	));
	p.add("visualizer_in_stereo", yes_no(
		visualizer_in_stereo, true
	));
	p.add("visualizer_sample_multiplier", assign_default<double>(
		visualizer_sample_multiplier, 1.0, [](double v) {
			lowerBoundCheck(v, 0.0);
			return v;
	}));
	p.add("visualizer_sync_interval", assign_default<unsigned>(
		visualizer_sync_interval, 30, [](unsigned v) {
			lowerBoundCheck(v, 10u);
			return boost::posix_time::seconds(v);
	}));
	p.add("visualizer_type", assign_default(
		visualizer_type, VisualizerType::Wave
	));
	p.add("visualizer_look", assign_default<std::string>(
		visualizer_chars, "●▮", [](std::string s) {
			auto result = ToWString(std::move(s));
			typedef std::wstring::size_type size_type;
			boundsCheck(result.size(), size_type(2), size_type(2));
			return result;
	}));
	p.add("visualizer_color", option_parser::worker([this](std::string v) {
		boost::sregex_token_iterator color(v.begin(), v.end(), boost::regex("\\w+")), end;
		for (; color != end; ++color)
		{
			try {
				visualizer_colors.push_back(boost::lexical_cast<NC::Color>(*color));
			} catch (boost::bad_lexical_cast &) {
				throw std::runtime_error("invalid color: " + *color);
			}
		}
		if (visualizer_colors.empty())
			throw std::runtime_error("empty list");
	}, [this] {
		visualizer_colors = { NC::Color::Blue, NC::Color::Cyan, NC::Color::Green, NC::Color::Yellow, NC::Color::Magenta, NC::Color::Red };
	}));
	p.add("system_encoding", assign_default<std::string>(
		system_encoding, "", [](std::string enc) {
#			ifdef HAVE_LANGINFO_H
			// try to autodetect system encoding
			if (enc.empty())
			{
				enc = nl_langinfo(CODESET);
				if (enc == "UTF-8") // mpd uses utf-8 by default so no need to convert
					enc.clear();
			}
#			endif // HAVE_LANGINFO_H
			return enc;
	}));
	p.add("playlist_disable_highlight_delay", assign_default<unsigned>(
		playlist_disable_highlight_delay, 5, [](unsigned v) {
			return boost::posix_time::seconds(v);
	}));
	p.add("message_delay_time", assign_default(
		message_delay_time, 5
	));
	p.add("song_list_format", assign_default<std::string>(
		song_list_format, "{%a - }{%t}|{$8%f$9}$R{$3(%l)$9}", [](std::string v) {
			return Format::parse(v);
	}));
	p.add("song_status_format", assign_default<std::string>(
		song_status_format, "{{%a{ \"%b\"{ (%y)}} - }{%t}}|{%f}", [this](std::string v) {
			const unsigned flags = Format::Flags::All ^ Format::Flags::OutputSwitch;
			// precompute wide format for status display
			song_status_wformat = Format::parse(ToWString(v), flags);
			return Format::parse(v, flags);
	}));
	p.add("song_library_format", assign_default<std::string>(
		song_library_format, "{%n - }{%t}|{%f}", [](std::string v) {
			return Format::parse(v);
	}));
	p.add("alternative_header_first_line_format", assign_default<std::string>(
		new_header_first_line, "$b$1$aqqu$/a$9 {%t}|{%f} $1$atqq$/a$9$/b", [](std::string v) {
			return Format::parse(ToWString(std::move(v)),
				Format::Flags::All ^ Format::Flags::OutputSwitch
			);
	}));
	p.add("alternative_header_second_line_format", assign_default<std::string>(
		new_header_second_line, "{{$4$b%a$/b$9}{ - $7%b$9}{ ($4%y$9)}}|{%D}", [](std::string v) {
			return Format::parse(ToWString(std::move(v)),
				Format::Flags::All ^ Format::Flags::OutputSwitch
			);
	}));
	p.add("now_playing_prefix", buffer(
		now_playing_prefix, NC::Buffer::init(NC::Format::Bold), [this](NC::Buffer buf) {
			now_playing_prefix_length = wideLength(ToWString(buf.str()));
			return buf;
	}));
	p.add("now_playing_suffix", buffer(
		now_playing_suffix, NC::Buffer::init(NC::Format::NoBold), [this](NC::Buffer buf) {
			now_playing_suffix_length = wideLength(ToWString(buf.str()));
			return buf;
	}));
	p.add("browser_playlist_prefix", buffer(
		browser_playlist_prefix, NC::Buffer::init(NC::Color::Red, "playlist", NC::Color::End, ' '), id_()
	));
	p.add("selected_item_prefix", buffer(
		selected_item_prefix, NC::Buffer::init(NC::Color::Magenta), [this](NC::Buffer buf) {
			selected_item_prefix_length = wideLength(ToWString(buf.str()));
			return buf;
	}));
	p.add("selected_item_suffix", buffer(
		selected_item_suffix, NC::Buffer::init(NC::Color::End), [this](NC::Buffer buf) {
			selected_item_suffix_length = wideLength(ToWString(buf.str()));
			return buf;
	}));
	p.add("modified_item_prefix", buffer(
		modified_item_prefix, NC::Buffer::init(NC::Color::Green, "> ", NC::Color::End), id_()
	));
	p.add("browser_sort_mode", assign_default(
		browser_sort_mode, SortMode::Name
	));
	p.add("browser_sort_format", assign_default<std::string>(
		browser_sort_format, "{%a - }{%t}|{%f} {(%l)}", [](std::string v) {
			return Format::parse(v, Format::Flags::Tag);
	}));
	p.add("song_window_title_format", assign_default<std::string>(
		song_window_title_format, "{%a - }{%t}|{%f}", [](std::string v) {
			return Format::parse(v, Format::Flags::Tag);
	}));
	p.add("song_columns_list_format", assign_default<std::string>(
		columns_format, "(20)[]{a} (6f)[green]{NE} (50)[white]{t|f:Title} (20)[cyan]{b} (7f)[magenta]{l}",
			[this](std::string v) {
				columns = generate_columns(v);
				song_columns_mode_format = columns_to_format(columns);
				return v;
	}));
	p.add("execute_on_song_change", assign_default(
		execute_on_song_change, ""
	));
	p.add("playlist_show_mpd_host", yes_no(
		playlist_show_mpd_host, false
	));
	p.add("playlist_show_remaining_time", yes_no(
		playlist_show_remaining_time, false
	));
	p.add("playlist_shorten_total_times", yes_no(
		playlist_shorten_total_times, false
	));
	p.add("playlist_separate_albums", yes_no(
		playlist_separate_albums, false
	));
	p.add("playlist_display_mode", assign_default(
		playlist_display_mode, DisplayMode::Columns
	));
	p.add("browser_display_mode", assign_default(
		browser_display_mode, DisplayMode::Classic
	));
	p.add("search_engine_display_mode", assign_default(
		search_engine_display_mode, DisplayMode::Classic
	));
	p.add("playlist_editor_display_mode", assign_default(
		playlist_editor_display_mode, DisplayMode::Classic
	));
	p.add("discard_colors_if_item_is_selected", yes_no(
		discard_colors_if_item_is_selected, true
	));
	p.add("incremental_seeking", yes_no(
		incremental_seeking, true
	));
	p.add("seek_time", assign_default(
		seek_time, 1
	));
	p.add("volume_change_step", assign_default(
		volume_change_step, 2
	));
	p.add("autocenter_mode", yes_no(
		autocenter_mode, false
	));
	p.add("centered_cursor", yes_no(
		centered_cursor, false
	));
	p.add("progressbar_look", assign_default<std::string>(
		progressbar, "=>", [](std::string s) {
			auto result = ToWString(std::move(s));
			typedef std::wstring::size_type size_type;
			boundsCheck(result.size(), size_type(2), size_type(3));
			// if two characters were specified, add third one (null)
			result.resize(3);
			return result;
	}));
	p.add("progressbar_boldness", yes_no(
		progressbar_boldness, true
	));
	p.add("default_place_to_search_in", option_parser::worker([this](std::string v) {
		if (v == "database")
			search_in_db = true;
		else if (v == "playlist")
			search_in_db = true;
		else
			throw std::runtime_error("invalid argument: " + v);
	}, defaults_to(search_in_db, true)
	));
	p.add("user_interface", assign_default(
		design, Design::Classic
	));
	p.add("data_fetching_delay", yes_no(
		data_fetching_delay, true
	));
	p.add("media_library_primary_tag", option_parser::worker([this](std::string v) {
		if (v == "artist")
			media_lib_primary_tag = MPD_TAG_ARTIST;
		else if (v == "album_artist")
			media_lib_primary_tag = MPD_TAG_ALBUM_ARTIST;
		else if (v == "date")
			media_lib_primary_tag = MPD_TAG_DATE;
		else if (v == "genre")
			media_lib_primary_tag = MPD_TAG_GENRE;
		else if (v == "composer")
			media_lib_primary_tag = MPD_TAG_COMPOSER;
		else if (v == "performer")
			media_lib_primary_tag = MPD_TAG_PERFORMER;
		else
			throw std::runtime_error("invalid argument: " + v);
	}, defaults_to(media_lib_primary_tag, MPD_TAG_ARTIST)
	));
	p.add("default_find_mode", option_parser::worker([this](std::string v) {
		if (v == "wrapped")
			wrapped_search = true;
		else if (v == "normal")
			wrapped_search = false;
		else
			throw std::runtime_error("invalid argument: " + v);
	}, defaults_to(wrapped_search, true)
	));
	p.add("default_tag_editor_pattern", assign_default(
		pattern, "%n - %t"
	));
	p.add("header_visibility", yes_no(
		header_visibility, true
	));
	p.add("statusbar_visibility", yes_no(
		statusbar_visibility, true
	));
	p.add("titles_visibility", yes_no(
		titles_visibility, true
	));
	p.add("header_text_scrolling", yes_no(
		header_text_scrolling, true
	));
	p.add("cyclic_scrolling", yes_no(
		use_cyclic_scrolling, false
	));
	p.add("lines_scrolled", assign_default(
		lines_scrolled, 2
	));
	p.add("follow_now_playing_lyrics", yes_no(
		now_playing_lyrics, false
	));
	p.add("fetch_lyrics_for_current_song_in_background", yes_no(
		fetch_lyrics_in_background, false
	));
	p.add("store_lyrics_in_song_dir", yes_no(
		store_lyrics_in_song_dir, false
	));
	p.add("generate_win32_compatible_filenames", yes_no(
		generate_win32_compatible_filenames, true
	));
	p.add("allow_for_physical_item_deletion", yes_no(
		allow_for_physical_item_deletion, false
	));
	p.add("lastfm_preferred_language", assign_default(
		lastfm_preferred_language, "en"
	));
	p.add("space_add_mode", assign_default(
		space_add_mode, SpaceAddMode::AlwaysAdd
	));
	p.add("show_hidden_files_in_local_browser", yes_no(
		local_browser_show_hidden_files, false
	));
	p.add("screen_switcher_mode", option_parser::worker([this](std::string v) {
		if (v == "previous")
			screen_switcher_previous = true;
		else
		{
			screen_switcher_previous = false;
			boost::sregex_token_iterator i(v.begin(), v.end(), boost::regex("\\w+")), j;
			for (; i != j; ++i)
			{
				auto screen = stringtoStartupScreenType(*i);
				if (screen != ScreenType::Unknown)
					screen_sequence.push_back(screen);
				else
					throw std::runtime_error("unknown screen: " + *i);
			}
		}
	}, [this] {
		screen_switcher_previous = false;
		screen_sequence = { ScreenType::Playlist, ScreenType::Browser };
	}));
	p.add("startup_screen", option_parser::worker([this](std::string v) {
		startup_screen_type = stringtoStartupScreenType(v);
		if (startup_screen_type == ScreenType::Unknown)
			throw std::runtime_error("unknown screen: " + v);
	}, defaults_to(startup_screen_type, ScreenType::Playlist)
	));
	p.add("startup_slave_screen", option_parser::worker([this](std::string v) {
		if (!v.empty())
		{
			startup_slave_screen_type = stringtoStartupScreenType(v);
			if (startup_slave_screen_type == ScreenType::Unknown)
				throw std::runtime_error("unknown slave screen: " + v);
		}
	}, defaults_to(startup_slave_screen_type, boost::none)
	));
	p.add("startup_slave_screen_focus", yes_no(
		startup_slave_screen_focus, false
	));
	p.add("locked_screen_width_part", assign_default<double>(
		locked_screen_width_part, 50.0, [](double v) {
			return v / 100;
	}));
	p.add("ask_for_locked_screen_width_part", yes_no(
		ask_for_locked_screen_width_part, true
	));
	p.add("jump_to_now_playing_song_at_start", yes_no(
		jump_to_now_playing_song_at_start, true
	));
	p.add("ask_before_clearing_playlists", yes_no(
		ask_before_clearing_playlists, true
	));
	p.add("ask_before_shuffling_playlists", yes_no(
		ask_before_shuffling_playlists, true
	));
	p.add("clock_display_seconds", yes_no(
		clock_display_seconds, false
	));
	p.add("display_volume_level", yes_no(
		display_volume_level, true
	));
	p.add("display_bitrate", yes_no(
		display_bitrate, false
	));
	p.add("display_remaining_time", yes_no(
		display_remaining_time, false
	));
	p.add("regular_expressions", option_parser::worker([this](std::string v) {
		if (v == "none")
			regex_type = boost::regex::literal;
		else if (v == "basic")
			regex_type = boost::regex::basic;
		else if (v == "extended")
			regex_type = boost::regex::extended;
		else if (v == "perl")
			regex_type = boost::regex::perl;
		else
			throw std::runtime_error("invalid argument: " + v);
		regex_type |= boost::regex::icase;
	}, defaults_to(regex_type, boost::regex::basic | boost::regex::icase)
	));
	p.add("ignore_leading_the", yes_no(
		ignore_leading_the, false
	));
	p.add("block_search_constraints_change_if_items_found", yes_no(
		block_search_constraints_change, true
	));
	p.add("mouse_support", yes_no(
		mouse_support, true
	));
	p.add("mouse_list_scroll_whole_page", yes_no(
		mouse_list_scroll_whole_page, true
	));
	p.add("empty_tag_marker", assign_default(
		empty_tag, "<empty>"
	));
	p.add("tags_separator", assign_default(
		MPD::Song::TagsSeparator, " | "
	));
	p.add("tag_editor_extended_numeration", yes_no(
		tag_editor_extended_numeration, false
	));
	p.add("media_library_sort_by_mtime", yes_no(
		media_library_sort_by_mtime, false
	));
	p.add("enable_window_title", [this]() {
		// Consider this variable only if TERM variable is available
		// and we're not in emacs terminal nor tty (through any wrapper
		// like screen).
		auto term = getenv("TERM");
		if (term != nullptr
		 && strstr(term, "linux") == nullptr
		 && strncmp(term, "eterm", const_strlen("eterm")))
			return yes_no(set_window_title, true);
		else
		{
			set_window_title = false;
			return option_parser::worker([](std::string) {}, [] {
				std::clog << "Terminal doesn't support window title, skipping 'enable_window_title'.\n";
			});
		}
	}());
	p.add("search_engine_default_search_mode", assign_default<unsigned>(
		search_engine_default_search_mode, 1, [](unsigned v) {
			boundsCheck(v, 1u, 3u);
			return --v;
	}));
	p.add("external_editor", assign_default(
		external_editor, "nano"
	));
	p.add("use_console_editor", yes_no(
		use_console_editor, true
	));
	p.add("colors_enabled", yes_no(
		colors_enabled, true
	));
	p.add("empty_tag_color", assign_default(
		empty_tags_color, NC::Color::Cyan
	));
	p.add("header_window_color", assign_default(
		header_color, NC::Color::Default
	));
	p.add("volume_color", assign_default(
		volume_color, NC::Color::Default
	));
	p.add("state_line_color", assign_default(
		state_line_color, NC::Color::Default
	));
	p.add("state_flags_color", assign_default(
		state_flags_color, NC::Color::Default
	));
	p.add("main_window_color", assign_default(
		main_color, NC::Color::Yellow
	));
	p.add("color1", assign_default(
		color1, NC::Color::White
	));
	p.add("color2", assign_default(
		color2, NC::Color::Green
	));
	p.add("main_window_highlight_color", assign_default(
		main_highlight_color, NC::Color::Yellow
	));
	p.add("progressbar_color", assign_default(
		progressbar_color, NC::Color::Black
	));
	p.add("progressbar_elapsed_color", assign_default(
		progressbar_elapsed_color, NC::Color::Green
	));
	p.add("statusbar_color", assign_default(
		statusbar_color, NC::Color::Default
	));
	p.add("alternative_ui_separator_color", assign_default(
		alternative_ui_separator_color, NC::Color::Black
	));
	p.add("active_column_color", assign_default(
		active_column_color, NC::Color::Red
	));
	p.add("window_border_color", border(
		window_border, NC::Color::Green
	));
	p.add("active_window_border", border(
		active_window_border, NC::Color::Red
	));

	return std::all_of(
		config_paths.begin(),
		config_paths.end(),
		[&](const std::string &config_path) {
			std::ifstream f(config_path);
			if (f.is_open())
				std::clog << "Reading configuration from " << config_path << "...\n";
			return p.run(f, ignore_errors);
		}
	) && p.initialize_undefined(ignore_errors);
}

/* vim: set tabstop=4 softtabstop=4 shiftwidth=4 noexpandtab : */

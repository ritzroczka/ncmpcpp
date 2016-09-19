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

#ifndef NCMPCPP_PLAYLIST_EDITOR_H
#define NCMPCPP_PLAYLIST_EDITOR_H

#include <boost/date_time/posix_time/posix_time_types.hpp>

#include "interfaces.h"
#include "regex_filter.h"
#include "screen.h"
#include "song_list.h"

struct PlaylistEditor: Screen<NC::Window *>, HasColumns, HasSongs, Searchable, Tabbable
{
	PlaylistEditor();
	
	virtual void switchTo() OVERRIDE;
	virtual void resize() OVERRIDE;
	
	virtual std::wstring title() OVERRIDE;
	virtual ScreenType type() OVERRIDE { return ScreenType::PlaylistEditor; }
	
	virtual void refresh() OVERRIDE;
	virtual void update() OVERRIDE;
	
	virtual int windowTimeout() OVERRIDE;

	virtual void mouseButtonPressed(MEVENT me) OVERRIDE;
	
	virtual bool isLockable() OVERRIDE { return true; }
	virtual bool isMergable() OVERRIDE { return true; }
	
	// Searchable implementation
	virtual bool allowsSearching() OVERRIDE;
	virtual void setSearchConstraint(const std::string &constraint) OVERRIDE;
	virtual void clearConstraint() OVERRIDE;
	virtual bool find(SearchDirection direction, bool wrap, bool skip_current) OVERRIDE;
	
	// HasSongs implementation
	virtual bool itemAvailable() OVERRIDE;
	virtual bool addItemToPlaylist(bool play) OVERRIDE;
	virtual std::vector<MPD::Song> getSelectedSongs() OVERRIDE;
	
	// HasColumns implementation
	virtual bool previousColumnAvailable() OVERRIDE;
	virtual void previousColumn() OVERRIDE;
	
	virtual bool nextColumnAvailable() OVERRIDE;
	virtual void nextColumn() OVERRIDE;
	
	// private members
	void updateTimer();

	void requestPlaylistsUpdate() { m_playlists_update_requested = true; }
	void requestContentsUpdate() { m_content_update_requested = true; }
	
	virtual void Locate(const MPD::Playlist &playlist);
	
	NC::Menu<MPD::Playlist> Playlists;
	SongMenu Content;
	
private:
	bool m_playlists_update_requested;
	bool m_content_update_requested;

	boost::posix_time::ptime m_timer;

	const int m_window_timeout;
	const boost::posix_time::time_duration m_fetching_delay;

	Regex::Filter<MPD::Playlist> m_playlists_search_predicate;
	Regex::Filter<MPD::Song> m_content_search_predicate;
};

extern PlaylistEditor *myPlaylistEditor;

#endif // NCMPCPP_PLAYLIST_EDITOR_H


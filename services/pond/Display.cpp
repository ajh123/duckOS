/*
    This file is part of duckOS.

    duckOS is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    duckOS is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with duckOS.  If not, see <https://www.gnu.org/licenses/>.

    Copyright (c) Byteduck 2016-2020. All rights reserved.
*/

#include "Display.h"
#include <unistd.h>
#include <cstdio>
#include <sys/ioctl.h>
#include <kernel/device/VGADevice.h>
#include <cstring>

Display::Display(): _dimensions({0, 0, 0, 0}) {
	framebuffer_fd = open("/dev/fb0", O_RDWR);
	if(framebuffer_fd < -1) {
		perror("Failed to open framebuffer");
		return;
	}

	if(ioctl(framebuffer_fd, IO_VIDEO_WIDTH, &_dimensions.width) < 0) {
		perror("Failed to get framebuffer width");
		return;
	}

	if(ioctl(framebuffer_fd, IO_VIDEO_HEIGHT, &_dimensions.height) < 0) {
		perror("Failed to get framebuffer height");
		return;
	}

	Color* buffer;

	if(ioctl(framebuffer_fd, IO_VIDEO_MAP, &buffer) < 0) {
		perror("Failed to map framebuffer");
		return;
	}

	_framebuffer = {buffer, _dimensions.width, _dimensions.height};

	printf("Framebuffer opened and mapped (%d x %d).\n", _dimensions.width, _dimensions.height);
}

Rect Display::dimensions() {
	return _dimensions;
}

Framebuffer Display::framebuffer() {
	return _framebuffer;
}

void Display::clear(Color color) {
	size_t framebuffer_size = _dimensions.area();
	for(size_t i = 0; i < framebuffer_size; i++) {
		_framebuffer.buffer[i] = color;
	}
}

void Display::set_root_window(Window* window) {
	_root_window = window;
}

void Display::set_mouse_window(Mouse* window) {
	_mouse_window = window;
}

void Display::add_window(Window* window) {
	_windows.push_back(window);
}

void Display::remove_window(Window* window) {
	for(size_t i = 0; i < _windows.size(); i++) {
		if(_windows[i] == window) {
			_windows.erase(_windows.begin() + i);
			return;
		}
	}
}

void Display::invalidate(const Rect& rect) {
	if(!rect.empty())
		invalid_areas.push_back(rect);
}

void Display::repaint() {
	auto fb = _root_window->framebuffer();

	for(auto& area : invalid_areas) {
		// Fill the invalid area with the background color.
		fb.fill(area, {50, 50, 50});

		// See if each window overlaps the invalid area.
		for(auto window : _windows) {
			//Don't bother with the mouse window, we draw it separately so it's always on top
			if(window == _mouse_window)
				continue;

			Rect window_vabs = window->visible_absolute_rect();
			if(window_vabs.collides(area)) {
				//If it does, redraw the intersection of the window in question and the invalid area
				Rect window_abs = window->absolute_rect();
				Rect overlap_abs = area.overlapping_area(window_vabs);
				fb.copy(window->framebuffer(), overlap_abs.transform({-window_abs.x, -window_abs.y}), overlap_abs.position());
			}
		}
	}

	//Draw the mouse.
	fb.copy(_mouse_window->framebuffer(), {0, 0, _mouse_window->rect().width, _mouse_window->rect().height}, _mouse_window->absolute_rect().position());

	memcpy(_framebuffer.buffer, fb.buffer, sizeof(Color) * _framebuffer.width * _framebuffer.height);
	invalid_areas.resize(0);
}

void Display::move_to_front(Window* window) {
	for (auto it = _windows.begin(); it != _windows.end(); it++) {
		if(*it == window) {
			_windows.erase(it);
			_windows.push_back(window);
			window->invalidate();
			return;
		}
	}
}

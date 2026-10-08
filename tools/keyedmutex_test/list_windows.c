// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
// Lists a Wine prefix's windows with their owning process, class, styles and
// size, children included. Read-only.
//
//   x86_64-w64-mingw32-gcc -O1 -o list_windows.exe list_windows.c -luser32
//   list_windows.exe [only visible: 1]
#include <windows.h>

#include <stdio.h>
#include <stdlib.h>

static int only_visible;

static void
print_window(HWND hwnd, int depth)
{
	DWORD pid = 0;
	GetWindowThreadProcessId(hwnd, &pid);
	char klass[64] = "", title[64] = "";
	GetClassNameA(hwnd, klass, sizeof(klass));
	GetWindowTextA(hwnd, title, sizeof(title));
	RECT rect;
	GetWindowRect(hwnd, &rect);
	LONG style = GetWindowLongA(hwnd, GWL_STYLE), ex = GetWindowLongA(hwnd, GWL_EXSTYLE);
	if (only_visible && !(style & WS_VISIBLE)) {
		return;
	}
	printf("%*s%p pid %lu %-24s '%s' %ldx%ld style %08lx ex %08lx%s%s%s\n", depth * 2, "", (void *)hwnd,
	       (unsigned long)pid, klass, title, rect.right - rect.left, rect.bottom - rect.top, (unsigned long)style,
	       (unsigned long)ex, (ex & WS_EX_LAYERED) ? " LAYERED" : "", (ex & WS_EX_NOREDIRECTIONBITMAP) ? " NOREDIR" : "",
	       (style & WS_POPUP) ? " POPUP" : "");
}

// EnumChildWindows visits all descendants, in tree order; indent by depth.
static BOOL CALLBACK
child(HWND hwnd, LPARAM top_level)
{
	int depth = 0;
	for (HWND up = hwnd; up && up != (HWND)top_level; up = GetAncestor(up, GA_PARENT)) {
		++depth;
	}
	print_window(hwnd, depth);
	return TRUE;
}

static BOOL CALLBACK
top(HWND hwnd, LPARAM unused)
{
	print_window(hwnd, 0);
	EnumChildWindows(hwnd, child, (LPARAM)hwnd);
	return TRUE;
}

int
main(int argc, char **argv)
{
	only_visible = argc > 1 && atoi(argv[1]);
	EnumWindows(top, 0);
	return 0;
}

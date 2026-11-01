// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Shows the Compatibility page in a game's Properties in macOS Steam, in memory only
// (docs/steam-play.md, The Compatibility page). Built into the compat gate library, which
// Steam Play.app loads into Steam and which Steam's processes inherit.
//
// Steam's interface is web code in steamui/. The Properties window adds its Compatibility
// page only on Linux:
//
//     (0,p.CI)()&&Rr.push({title:(0,d.we)("#AppProperties_CompatibilityPage"),...
//
// where CI() is PLATFORM=="linux", a check the interface uses in 23 places. When a Steam
// web helper opens (or stats) steamui/sp.js, this hands it a copy in which only the checks
// directly before "#AppProperties_CompatibilityPage" read (!0      ), so the rest of
// Steam still sees macOS. On macOS the page's tool list (CompatManager.GetCompatTools)
// comes back empty, so the copy also logs what the page receives ("mwxr:" in the web
// helper's console) and, when it is empty, offers the macos-wine-xr-proton tool, whose
// selection goes to Steam's own CompatManager.SpecifyCompatTool. The copy is made once
// per sp.js (by size and time) in ~/Library/Caches/macos-wine-xr/steamui; nothing of
// Steam's is changed on disk. If the code no longer has the expected shape, that part is
// left out. MWXR_STEAM_COMPATIBILITY_PAGE=0 turns this off.

#include <ctype.h>
#include <errno.h>
#include <libgen.h>
#include <mach-o/dyld.h>
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

void mwxr_gate_log(const char *fmt, ...);

#define UI_FILE "/steamui/sp.js"
#define MARKER "(\"#AppProperties_CompatibilityPage\")"

static int is_ident(char c)
{
	return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '$';
}

// Rewrites each "(0,X.CI)()" that directly guards a Compatibility page entry as
// "(!0" + spaces + ")", in place. Returns how many it rewrote.
int mwxr_patch_compatibility_page(char *text, size_t len)
{
	static const char call[] = ".CI)()&&";
	const size_t marker_len = strlen(MARKER), call_len = strlen(call);
	int count = 0;
	for (char *m = text; (m = memmem(m, len - (size_t)(m - text), MARKER, marker_len)) != NULL;
	     m += marker_len) {
		// The guard sits within the 160 bytes before the page's title.
		char *window = m - text > 160 ? m - 160 : text, *found = NULL;
		for (char *c = window; c + call_len <= m; c++)
			if (memcmp(c, call, call_len) == 0)
				found = c;
		if (!found)
			continue;
		// Back from ".CI" over the module name to "(0,".
		char *start = found;
		while (start > window && is_ident(start[-1]) && found - start < 8)
			start--;
		if (start - window < 3 || memcmp(start - 3, "(0,", 3) != 0 || start == found)
			continue;
		start -= 3;
		char *end = found + strlen(".CI)()");  // just past the guard's "()"
		size_t n = (size_t)(end - start);
		if (n < 5)
			continue;
		memset(start, ' ', n);
		memcpy(start, "(!0", 3);
		start[n - 1] = ')';
		count++;
	}
	return count;
}

struct buffer {
	char *data;
	size_t len, cap;
};

static int append(struct buffer *b, const char *s, size_t n)
{
	if (b->len + n > b->cap) {
		size_t cap = b->cap ? b->cap : 4096;
		while (cap < b->len + n)
			cap *= 2;
		char *grown = realloc(b->data, cap);
		if (!grown)
			return 0;
		b->data = grown;
		b->cap = cap;
	}
	memcpy(b->data + b->len, s, n);
	b->len += n;
	return 1;
}

// The page's tool list and selection go through a React hook in sp.js:
//     async()=>{const H=await m.GetCompatTools({appid:v});...;return H.Body().toObject()}
//     ...E=(0,y.I)({queryKey:x,queryFn:j}),C=...{m.SpecifyCompatTool({appid:v,tool_name:H})}...
// macOS Steam has no handler for either message ("no handler for message:
// CompatManager.GetCompatTools#1"), so both become requests to steamloopback.host that
// this library answers in the web helper (answer_mwxr below):
//     GET /mwxr/compat/<appid>.json             the game's tools and current choice
//     GET /mwxr/select/<appid>/<tool>.json      a choice, written to the games file
// and a choice refetches the list. Returns a new buffer (its length in *outlen), or NULL
// if the code does not have that shape.
static const char *ident_before(const char *p, const char *floor, size_t *len)
{
	const char *q = p;
	while (q > floor && is_ident(q[-1]) && p - q < 16)
		q--;
	*len = (size_t)(p - q);
	return q;
}

static size_t ident_len(const char *p, const char *end)
{
	size_t n = 0;
	while (p + n < end && is_ident(p[n]) && n < 16)
		n++;
	return n;
}

char *mwxr_patch_compat_service(const char *text, size_t len, size_t *outlen)
{
	static const char get[] = ".GetCompatTools({appid:", spec[] = ".SpecifyCompatTool({appid:",
	                  key[] = "({queryKey:";
	const char *end = text + len;
	const char *g = memmem(text, len, get, strlen(get));
	if (!g || memmem(g + 1, (size_t)(end - g - 1), get, strlen(get)))
		return NULL;  // exactly one
	// "{const H=await m" before it
	size_t mlen, hlen;
	const char *m = ident_before(g, text, &mlen);
	if (!mlen || m - text < 32 || memcmp(m - 6, "await ", 6) != 0 || m[-7] != '=')
		return NULL;
	const char *h = ident_before(m - 7, text, &hlen);
	if (!hlen || memcmp(h - 7, "{const ", 7) != 0)
		return NULL;
	const char *q_start = h - 6;  // just after "{"
	const char *v = g + strlen(get);
	size_t vlen = ident_len(v, end);
	char tail[64];
	snprintf(tail, sizeof(tail), "return %.*s.Body().toObject()}", (int)hlen, h);
	const char *q_end = memmem(g, (size_t)(end - g) < 400 ? (size_t)(end - g) : 400, tail, strlen(tail));
	if (!vlen || !q_end)
		return NULL;
	q_end += strlen(tail) - 1;  // the function's closing "}"

	// The query result's name: "<E>=(0,y.I)({queryKey:" after the query function.
	const char *k = memmem(q_end, (size_t)(end - q_end) < 300 ? (size_t)(end - q_end) : 300, key, strlen(key));
	if (!k)
		return NULL;
	const char *e = k;
	while (e > q_end && *e != '=')
		e--;
	size_t elen;
	const char *ev = ident_before(e, q_end, &elen);
	// "<m>.SpecifyCompatTool({appid:<v>,tool_name:<W>})"
	const char *sp = memmem(k, (size_t)(end - k) < 300 ? (size_t)(end - k) : 300, spec, strlen(spec));
	if (!elen || !sp || (size_t)(sp - text) < mlen || memcmp(sp - mlen, m, mlen) != 0)
		return NULL;
	const char *s_start = sp - mlen, *w = sp + strlen(spec) + vlen;
	if (memcmp(w, ",tool_name:", 11) != 0)
		return NULL;
	w += 11;
	size_t wlen = ident_len(w, end);
	if (!wlen || memcmp(w + wlen, "})", 2) != 0)
		return NULL;
	const char *s_end = w + wlen + 2;

	char query[512], select[512];
	int nq = snprintf(query, sizeof(query),
	    "const mwxrR=await fetch(\"/mwxr/compat/\"+(%1$.*2$s??0)+\".json?\"+Date.now());"
	    "if(!mwxrR.ok)throw new Error(\"macos-wine-xr compat tools: \"+mwxrR.status);return await mwxrR.json()",
	    v, (int)vlen);
	int ns = snprintf(select, sizeof(select),
	    "fetch(\"/mwxr/select/\"+(%1$.*2$s??0)+\"/\"+encodeURIComponent(%3$.*4$s||\"default\")+\".json?\"+Date.now())"
	    ".then(()=>%5$.*6$s.refetch())",
	    v, (int)vlen, w, (int)wlen, ev, (int)elen);
	if (nq <= 0 || ns <= 0 || (size_t)nq >= sizeof(query) || (size_t)ns >= sizeof(select))
		return NULL;
	struct buffer out = {0};
	int ok = append(&out, text, (size_t)(q_start - text)) && append(&out, query, (size_t)nq) &&
	         append(&out, q_end, (size_t)(s_start - q_end)) && append(&out, select, (size_t)ns) &&
	         append(&out, s_end, (size_t)(end - s_end));
	if (!ok) {
		free(out.data);
		return NULL;
	}
	*outlen = out.len;
	return out.data;
}

// The tool dropdown's description, (0,e.jsx)(I,{toolActive:j,response:D.data}), becomes
// "Applies when Steam restarts" with a Restart Steam button while the answer says a
// choice is not in use yet (mwxr_pending). Steam's restart ends the client with exit code
// 42 and Steam Play.app starts it again with the game list brought up to date.
char *mwxr_patch_restart_note(const char *text, size_t len, size_t *outlen)
{
	static const char key[] = ",{toolActive:", desc[] = "description:(0,";
	const char *end = text + len;
	const char *k = memmem(text, len, key, strlen(key));
	if (!k || memmem(k + 1, (size_t)(end - k - 1), key, strlen(key)))
		return NULL;
	// back to "description:(0,<J>.jsx)(<I>"
	const char *d = k;
	while (d > text && k - d < 40 && memcmp(d, desc, strlen(desc)) != 0)
		d--;
	if (memcmp(d, desc, strlen(desc)) != 0)
		return NULL;
	const char *jv = d + strlen(desc);
	size_t jlen = ident_len(jv, end);
	if (!jlen || memcmp(jv + jlen, ".jsx)(", 6) != 0)
		return NULL;
	// "...,response:<D>.data})"
	const char *resp = memmem(k, 80, ",response:", 10);
	if (!resp)
		return NULL;
	const char *dv = resp + 10;
	size_t dlen = ident_len(dv, end);
	if (!dlen || memcmp(dv + dlen, ".data})", 7) != 0)
		return NULL;
	const char *orig = d + strlen("description:"), *orig_end = dv + dlen + 7;
	char note[1024];
	int n = snprintf(note, sizeof(note),
	    "description:%2$.*3$s.data?.mwxr_pending?(0,%1$.*4$s.jsxs)(\"span\",{children:[\"Applies when Steam restarts. \","
	    "(0,%1$.*4$s.jsx)(\"button\",{className:\"DialogButton\",style:{display:\"inline-block\",width:\"auto\","
	    "minWidth:0,padding:\"0 12px\",marginLeft:\"8px\"},onClick:mwxrEv=>{mwxrEv.stopPropagation();"
	    "SteamClient.User.StartRestart(!1)},children:\"Restart Steam\"})]}):",
	    jv, dv, (int)dlen, (int)jlen);
	if (n <= 0 || (size_t)n >= sizeof(note))
		return NULL;
	struct buffer out = {0};
	int ok = append(&out, text, (size_t)(d - text)) && append(&out, note, (size_t)n) &&
	         append(&out, orig, (size_t)(orig_end - orig)) && append(&out, orig_end, (size_t)(end - orig_end));
	if (!ok) {
		free(out.data);
		return NULL;
	}
	*outlen = out.len;
	return out.data;
}

// Steam marks a game whose macOS build is 32-bit only with is_invalid_os_type: a "no entry"
// icon in the library list, a greyed entry and a "unable to run 32-bit games" banner on its
// page. Steam Play runs such a game's Windows build, so each "<X>.is_invalid_os_type" on an
// app overview becomes "(<X>.is_invalid_os_type&&!globalThis.mwxrMapped?.has(<X>.appid))",
// and the start of the file loads that set, once, from /mwxr/mapped.json: the games Steam
// mapped to the tool when it started. Returns a new buffer, or NULL if none was found.
char *mwxr_patch_invalid_os_type(const char *text, size_t len, size_t *outlen)
{
	static const char field[] = ".is_invalid_os_type",
	    prelude[] = "globalThis.mwxrMapped=(()=>{try{const x=new XMLHttpRequest();"
	                "x.open(\"GET\",\"/mwxr/mapped.json\",!1);x.send();return new Set(JSON.parse(x.responseText))}"
	                "catch(e){return new Set}})();\n";
	const char *end = text + len, *from = text;
	struct buffer out = {0};
	int count = 0;
	if (!append(&out, prelude, strlen(prelude)))
		return NULL;
	// from: copied up to here; at: searched up to here.
	for (const char *f, *at = text; (f = memmem(at, (size_t)(end - at), field, strlen(field))) != NULL;
	     at = f + strlen(field)) {
		size_t vlen;
		const char *v = ident_before(f, from, &vlen);
		// a plain variable (not "a.b.is_invalid_os_type"), and not a longer field name
		if (!vlen || (v > text && (v[-1] == '.' || is_ident(v[-1]))) || is_ident(f[strlen(field)]))
			continue;
		char repl[160];
		int n = snprintf(repl, sizeof(repl), "(%.*s.is_invalid_os_type&&!globalThis.mwxrMapped?.has(%.*s.appid))",
		                 (int)vlen, v, (int)vlen, v);
		if (n <= 0 || (size_t)n >= sizeof(repl) || !append(&out, from, (size_t)(v - from)) ||
		    !append(&out, repl, (size_t)n)) {
			free(out.data);
			return NULL;
		}
		from = f + strlen(field);
		count++;
	}
	if (!count || !append(&out, from, (size_t)(end - from))) {
		free(out.data);
		return NULL;
	}
	*outlen = out.len;
	return out.data;
}

// In the library list, a mapped game gets Steam's monochrome Windows logo (icon Xz0, the
// same module as the "no entry" icon WOg) where the 32-bit mark would be, with the same
// styling (16 px, grey) and a tooltip. After mwxr_patch_invalid_os_type, the list's
//     (Z.is_invalid_os_type&&!...has(Z.appid))?be=(0,e.jsx)(j.he,{toolTipContent:(0,S.we)(
//     "#GameList_Entry_Invalid_OSType"),className:O().InvalidOSType,children:(0,e.jsx)(i.WOg,{})}):
// gains "globalThis.mwxrMapped?.has(Z.appid)?be=<the same element, with Xz0>:" after it.
char *mwxr_patch_windows_logo(const char *text, size_t len, size_t *outlen)
{
	static const char tip[] = "(\"#GameList_Entry_Invalid_OSType\"),className:", appid[] = ".appid))?be=",
	                  icon[] = ".WOg,{})})";
	const char *end = text + len;
	for (const char *t = text; (t = memmem(t, (size_t)(end - t), tip, strlen(tip))) != NULL; t += strlen(tip)) {
		// "?be=" within 80 bytes before: the element starts after it
		const char *b = t;
		while (b > text && t - b < 80 && memcmp(b, appid, strlen(appid)) != 0)
			b--;
		if (memcmp(b, appid, strlen(appid)) != 0)
			continue;
		size_t zlen;
		const char *z = ident_before(b, text, &zlen);
		const char *elem = b + strlen(appid);
		const char *i = memmem(t, (size_t)(end - t) < 200 ? (size_t)(end - t) : 200, icon, strlen(icon));
		if (!zlen || !i || i[strlen(icon)] != ':')
			continue;
		const char *elem_end = i + strlen(icon);  // just past "})"
		// toolTipContent:<expr> ends at the tip's "),className:"
		static const char tt[] = "toolTipContent:";
		const char *tc = memmem(elem, (size_t)(t - elem), tt, strlen(tt));
		if (!tc)
			continue;
		tc += strlen(tt);
		const char *tc_end = t + strlen("(\"#GameList_Entry_Invalid_OSType\")");  // the closing ")" of the call
		struct buffer out = {0};
		int ok = append(&out, text, (size_t)(elem_end + 1 - text)) &&  // through the ':'
		         append(&out, "globalThis.mwxrMapped?.has(", 27) && append(&out, z, zlen) &&
		         append(&out, ".appid)?be=", 11) && append(&out, elem, (size_t)(tc - elem)) &&
		         append(&out, "\"Steam Play: runs the Windows version with Wine\"", 48) &&
		         append(&out, tc_end, (size_t)(i - tc_end)) && append(&out, ".Xz0,{})}):", 11) &&
		         append(&out, elem_end + 1, (size_t)(end - elem_end - 1));
		if (!ok) {
			free(out.data);
			return NULL;
		}
		*outlen = out.len;
		return out.data;
	}
	return NULL;
}

// The patched copy of the sp.js at path, made if needed; NULL to use the original.
static const char *patched_copy(const char *path, char *out, size_t outlen)
{
	const char *home = getenv("HOME");
	struct stat st;
	if (!home || stat(path, &st) != 0)
		return NULL;
	char dir[PATH_MAX], nomatch[PATH_MAX];
	snprintf(dir, sizeof(dir), "%s/Library/Caches/macos-wine-xr/steamui", home);
	// v8: the page, its tool service, the restart note, 32-bit macOS marks, the Windows logo. A new version of the patch gets new file names.
	snprintf(out, outlen, "%s/sp-v8-%lld-%ld.js", dir, (long long)st.st_size, (long)st.st_mtimespec.tv_sec);
	snprintf(nomatch, sizeof(nomatch), "%s.unchanged", out);
	struct stat have;
	if (stat(out, &have) == 0)
		return out;
	if (stat(nomatch, &have) == 0)
		return NULL;

	char *text = malloc((size_t)st.st_size);
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	ssize_t got = 0;
	if (fd >= 0) {
		while (got < st.st_size) {
			ssize_t n = read(fd, text + got, (size_t)(st.st_size - got));
			if (n <= 0)
				break;
			got += n;
		}
		close(fd);
	}
	if (!text || got != st.st_size) {
		free(text);
		return NULL;
	}
	int count = mwxr_patch_compatibility_page(text, (size_t)st.st_size);
	size_t size = (size_t)st.st_size;
	int tools = 0;
	if (count > 0) {
		size_t before = size;
		char *grown = mwxr_patch_compat_service(text, size, &size);
		if (grown) {
			free(text);
			text = grown;
			tools = 1;
			before = size;
			char *noted = mwxr_patch_restart_note(text, size, &size);
			if (noted) {
				free(text);
				text = noted;
			} else {
				size = before;
			}
			before = size;
			char *valid = mwxr_patch_invalid_os_type(text, size, &size);
			if (valid) {
				free(text);
				text = valid;
				before = size;
				char *logo = mwxr_patch_windows_logo(text, size, &size);
				if (logo) {
					free(text);
					text = logo;
				} else {
					size = before;
				}
			} else {
				size = before;
			}
		} else {
			size = before;
		}
	}
	char parent[PATH_MAX];
	snprintf(parent, sizeof(parent), "%s/Library/Caches/macos-wine-xr", home);
	mkdir(parent, 0700);
	mkdir(dir, 0700);
	if (count == 0) {
		mwxr_gate_log("%s: no Compatibility page guard found; using it unchanged", path);
		int marker = open(nomatch, O_WRONLY | O_CREAT | O_CLOEXEC, 0600);
		if (marker >= 0)
			close(marker);
		free(text);
		return NULL;
	}
	char tmp[PATH_MAX];
	snprintf(tmp, sizeof(tmp), "%s.%d.tmp", out, getpid());
	int w = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
	int ok = w >= 0 && write(w, text, size) == (ssize_t)size;
	if (w >= 0)
		close(w);
	free(text);
	if (!ok || rename(tmp, out) != 0) {
		unlink(tmp);
		return NULL;
	}
	mwxr_gate_log("%s: Compatibility page shown (%d guards, tool list %s) in %s", path, count,
	              tools ? "answered here" : "not found", out);
	return out;
}

// --- The tool service the page asks for (mwxr_patch_compat_service) -------------------

#define OUR_TOOL "macos-wine-xr-proton"
#define NO_TOOL "macos-native"

static void payload_path(char *out, size_t len, const char *name)
{
	snprintf(out, len, "%s/Library/Application Support/macos-wine-xr/steam-play/%s", getenv("HOME"), name);
}

static char *read_all(const char *path)
{
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return NULL;
	struct stat st;
	char *text = fstat(fd, &st) == 0 ? malloc((size_t)st.st_size + 1) : NULL;
	ssize_t got = text ? read(fd, text, (size_t)st.st_size) : -1;
	close(fd);
	if (got < 0) {
		free(text);
		return NULL;
	}
	text[got] = '\0';
	return text;
}

// The app ID a games or mapped line names: 123 (listed) or -123 (excluded); 0 if none.
static unsigned long line_appid(const char *line, int *excluded)
{
	while (*line == ' ' || *line == '\t')
		line++;
	*excluded = *line == '-';
	if (*excluded)
		line++;
	char *end;
	unsigned long id = strtoul(line, &end, 10);
	return end != line ? id : 0;
}

// "listed", "excluded" or "" for appid in the games file; from mapped, which Steam
// Play.app wrote when Steam started, the reason in "(...)" at the end of its line
// ("windows-only", "vr", "mac-32-bit", "listed") or "", and whether Steam is using our
// tool for it now (a line that starts with the app ID, not "#excluded <appid>").
static void game_state(unsigned long appid, char *choice, size_t clen, char *reason, size_t rlen, int *active)
{
	char path[PATH_MAX];
	*choice = *reason = '\0';
	*active = 0;
	payload_path(path, sizeof(path), "games");
	char *text = read_all(path);
	for (char *line = text; line && *line;) {
		char *next = strchr(line, '\n');
		int excluded;
		if (line_appid(line, &excluded) == appid)
			snprintf(choice, clen, "%s", excluded ? "excluded" : "listed");
		line = next ? next + 1 : NULL;
	}
	free(text);
	payload_path(path, sizeof(path), "mapped");
	text = read_all(path);
	for (char *line = text; line && *line;) {
		char *next = strchr(line, '\n');
		if (next)
			*next = '\0';
		int excluded, commented = strncmp(line, "#excluded ", 10) == 0;
		char *open_paren = strrchr(line, '('), *close_paren = strrchr(line, ')');
		if (line_appid(commented ? line + 10 : line, &excluded) == appid && open_paren && close_paren > open_paren) {
			snprintf(reason, rlen, "%.*s", (int)(close_paren - open_paren - 1), open_paren + 1);
			*active = !commented;
		}
		line = next ? next + 1 : NULL;
	}
	free(text);
}

static int write_file(const char *path, const char *text)
{
	char tmp[PATH_MAX];
	snprintf(tmp, sizeof(tmp), "%s.%d.tmp", path, getpid());
	int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
	size_t n = strlen(text);
	int ok = fd >= 0 && write(fd, text, n) == (ssize_t)n;
	if (fd >= 0)
		close(fd);
	if (!ok || rename(tmp, path) != 0) {
		unlink(tmp);
		return 0;
	}
	return 1;
}

// Rewrites the games file without appid's lines, then adds one for the choice: OUR_TOOL
// lists it, NO_TOOL excludes it, anything else leaves it automatic.
static int choose(unsigned long appid, const char *tool)
{
	char path[PATH_MAX];
	payload_path(path, sizeof(path), "games");
	char *text = read_all(path);
	struct buffer out = {0};
	for (char *line = text; line && *line;) {
		char *next = strchr(line, '\n');
		size_t n = next ? (size_t)(next - line + 1) : strlen(line);
		int excluded;
		if (line_appid(line, &excluded) != appid && !append(&out, line, n)) {
			free(text);
			free(out.data);
			return 0;
		}
		line = next ? next + 1 : NULL;
	}
	free(text);
	if (out.len && out.data[out.len - 1] != '\n')
		append(&out, "\n", 1);
	char add[96] = "";
	if (strcmp(tool, OUR_TOOL) == 0)
		snprintf(add, sizeof(add), "%lu   # chosen in Steam's Compatibility page\n", appid);
	else if (strcmp(tool, NO_TOOL) == 0)
		snprintf(add, sizeof(add), "-%lu  # chosen in Steam's Compatibility page: macOS build\n", appid);
	int ok = append(&out, add, strlen(add)) && append(&out, "", 1) && write_file(path, out.data);
	free(out.data);
	return ok;
}

// Answers /mwxr/... requests from the patched page with a generated file; NULL if path
// is not one.
static const char *answer_mwxr(const char *path, char *out, size_t outlen)
{
	const char *at = strstr(path, "/steamui/mwxr/");
	if (!at)
		return NULL;
	at += strlen("/steamui/mwxr/");
	char req[256];
	snprintf(req, sizeof(req), "%s", at);
	req[strcspn(req, "?")] = '\0';
	char dir[PATH_MAX];
	snprintf(dir, sizeof(dir), "%s/Library/Caches", getenv("HOME"));
	mkdir(dir, 0700);
	strlcat(dir, "/macos-wine-xr", sizeof(dir));
	mkdir(dir, 0700);
	strlcat(dir, "/steamui", sizeof(dir));
	mkdir(dir, 0700);
	unsigned long appid = 0;
	char tool[96] = "";
	if (strcmp(req, "mapped.json") == 0) {
		// The app IDs in mapped (lines that start with one), as a JSON array.
		char path[PATH_MAX];
		payload_path(path, sizeof(path), "mapped");
		char *text = read_all(path);
		struct buffer json = {0};
		int ok = append(&json, "[", 1);
		for (char *line = text; ok && line && *line;) {
			char *next = strchr(line, '\n');
			int excluded;
			unsigned long id = isdigit((unsigned char)*line) ? line_appid(line, &excluded) : 0;
			if (id) {
				char item[24];
				int n = snprintf(item, sizeof(item), "%s%lu", json.len > 1 ? "," : "", id);
				ok = append(&json, item, (size_t)n);
			}
			line = next ? next + 1 : NULL;
		}
		free(text);
		ok = ok && append(&json, "]", 2);
		snprintf(out, outlen, "%s/mapped.json", dir);
		ok = ok && write_file(out, json.data);
		free(json.data);
		return ok ? out : NULL;
	}
	if (sscanf(req, "compat/%lu.json", &appid) == 1) {
		char choice[16], reason[32], json[1024];
		int active;
		game_state(appid, choice, sizeof(choice), reason, sizeof(reason), &active);
		// windows-only, vr, mac-32-bit: mapped unless excluded.
		int automatic = reason[0] && strcmp(reason, "listed") != 0;
		const char *selected = strcmp(choice, "listed") == 0 ? OUR_TOOL : strcmp(choice, "excluded") == 0 ? NO_TOOL : "";
		int after = strcmp(choice, "listed") == 0 || (automatic && strcmp(choice, "excluded") != 0);
		snprintf(json, sizeof(json),
		    "{\"tools\":[{\"name\":\"" OUR_TOOL "\",\"display_name\":\"macos-wine-xr (Wine, DXMT, OpenXR)\",\"is_active\":%s},"
		    "{\"name\":\"" NO_TOOL "\",\"display_name\":\"None: the macOS build\",\"is_active\":%s}],"
		    "\"aliases\":[],\"selected_tool_identifier\":\"%s\",\"default_tool_identifier\":\"%s\","
		    "\"mwxr_pending\":%s}",
		    active ? "true" : "false", active ? "false" : "true", selected, automatic ? OUR_TOOL : NO_TOOL,
		    active != after ? "true" : "false");
		snprintf(out, outlen, "%s/compat-%lu.json", dir, appid);
		return write_file(out, json) ? out : NULL;
	}
	if (sscanf(req, "select/%lu/%95[^.].json", &appid, tool) == 2) {
		int ok = choose(appid, tool);
		mwxr_gate_log("Compatibility page: app %lu -> %s%s", appid, tool, ok ? "" : " (games file not written)");
		snprintf(out, outlen, "%s/select.json", dir);
		return write_file(out, ok ? "{\"ok\":true}" : "{\"ok\":false}") ? out : NULL;
	}
	return NULL;
}

// The process's executable name: steam_osx (the client, which serves steamui/ to the web
// helper and checks its own files against its install record) or "Steam Helper".
static const char *process_name(void)
{
	static char name[64];
	if (!name[0]) {
		char exe[PATH_MAX];
		uint32_t size = sizeof(exe);
		snprintf(name, sizeof(name), "%s", _NSGetExecutablePath(exe, &size) == 0 ? basename(exe) : "?");
	}
	return name;
}

enum access { BY_OPEN, BY_STAT };

// Opens of sp.js get the copy in every Steam process. A stat of it gets the copy only in
// the web helper: steam_osx checks its files' sizes against its install record, and a
// different size there makes it re-extract its package and restart at every start.
static const char *redirect(const char *path, int flags, enum access how, char *buf, size_t buflen)
{
	if (!path || (flags & O_ACCMODE) != O_RDONLY)
		return path;
	if (how == BY_STAT && strcmp(process_name(), "Steam Helper") != 0)
		return path;
	if (strstr(path, "/steamui/mwxr/")) {
		int saved = errno;
		const char *answer = answer_mwxr(path, buf, buflen);
		errno = saved;
		return answer ? answer : path;
	}
	size_t len = strlen(path), tail = strlen(UI_FILE);
	if (len < tail || memcmp(path + len - tail, UI_FILE, tail) != 0)
		return path;
	const char *off = getenv("MWXR_STEAM_COMPATIBILITY_PAGE");
	if (off && strcmp(off, "0") == 0)
		return path;
	int saved = errno;
	const char *copy = patched_copy(path, buf, buflen);
	// Rare: sp.js is read when Steam's interface cache is refreshed, after updates.
	mwxr_gate_log("%s %s sp.js -> %s", process_name(), how == BY_STAT ? "stat" : "open", copy ? "copy" : "original");
	errno = saved;
	return copy ? copy : path;
}

static int mwxr_open(const char *path, int flags, ...)
{
	mode_t mode = 0;
	if (flags & O_CREAT) {
		va_list ap;
		va_start(ap, flags);
		mode = (mode_t)va_arg(ap, int);
		va_end(ap);
	}
	char buf[PATH_MAX];
	return open(redirect(path, flags, BY_OPEN, buf, sizeof(buf)), flags, mode);
}

static int mwxr_openat(int dirfd, const char *path, int flags, ...)
{
	mode_t mode = 0;
	if (flags & O_CREAT) {
		va_list ap;
		va_start(ap, flags);
		mode = (mode_t)va_arg(ap, int);
		va_end(ap);
	}
	char buf[PATH_MAX];
	const char *p = path && path[0] == '/' ? redirect(path, flags, BY_OPEN, buf, sizeof(buf)) : path;
	return openat(dirfd, p, flags, mode);
}

// A reader may size the file before opening it, so stat sees the copy too.
static int mwxr_stat(const char *path, struct stat *st)
{
	char buf[PATH_MAX];
	return stat(redirect(path, O_RDONLY, BY_STAT, buf, sizeof(buf)), st);
}

static int mwxr_lstat(const char *path, struct stat *st)
{
	char buf[PATH_MAX];
	return lstat(redirect(path, O_RDONLY, BY_STAT, buf, sizeof(buf)), st);
}

static FILE *mwxr_fopen(const char *path, const char *mode)
{
	char buf[PATH_MAX];
	int flags = mode && mode[0] == 'r' && !strchr(mode, '+') ? O_RDONLY : O_RDWR;
	return fopen(redirect(path, flags, BY_OPEN, buf, sizeof(buf)), mode);
}

// dyld interposing: calls to these from every other image in the process come here.
__attribute__((used)) static const struct {
	const void *replacement, *original;
} interposers[] __attribute__((section("__DATA,__interpose"))) = {
	{(const void *)mwxr_open, (const void *)open},
	{(const void *)mwxr_openat, (const void *)openat},
	{(const void *)mwxr_fopen, (const void *)fopen},
	{(const void *)mwxr_stat, (const void *)stat},
	{(const void *)mwxr_lstat, (const void *)lstat},
};

#ifdef MWXR_UI_PATCH_MAIN
// Patches a copy of sp.js offline: steam_ui_patch IN OUT; prints the number of guards.
int main(int argc, char **argv)
{
	if (argc != 3)
		return 2;
	FILE *f = fopen(argv[1], "rb");
	if (!f)
		return 2;
	fseek(f, 0, SEEK_END);
	long n = ftell(f);
	fseek(f, 0, SEEK_SET);
	char *text = malloc((size_t)n);
	if (!text || fread(text, 1, (size_t)n, f) != (size_t)n)
		return 2;
	fclose(f);
	int count = mwxr_patch_compatibility_page(text, (size_t)n);
	size_t size = (size_t)n;
	char *grown = mwxr_patch_compat_service(text, size, &size);
	if (grown) {
		char *noted = mwxr_patch_restart_note(grown, size, &size);
		if (noted)
			grown = noted;
		char *valid = mwxr_patch_invalid_os_type(grown, size, &size);
		if (valid)
			grown = valid;
		char *logo = valid ? mwxr_patch_windows_logo(grown, size, &size) : NULL;
		if (logo)
			grown = logo;
	}
	f = fopen(argv[2], "wb");
	if (!f || fwrite(grown ? grown : text, 1, size, f) != size)
		return 2;
	fclose(f);
	printf("%d guards, tool service %s\n", count, grown ? "patched" : "not found");
	return count > 0 ? 0 : 1;
}

void mwxr_gate_log(const char *fmt, ...)
{
	(void)fmt;
}
#endif

#ifdef MWXR_UI_HANDLER_MAIN
// Answers one /mwxr/ request path offline and prints the answer: steam_ui_handler PATH.
void mwxr_gate_log(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
}

int main(int argc, char **argv)
{
	char buf[PATH_MAX];
	const char *answer = argc == 2 ? answer_mwxr(argv[1], buf, sizeof(buf)) : NULL;
	char *text = answer ? read_all(answer) : NULL;
	printf("%s\n", text ? text : "(no answer)");
	return text ? 0 : 1;
}
#endif

// The native mod API's runtime (ext/modapi/README.md, sdk/docs/MODDING.md): finds
// the mods under mods/ (a folder with a mod.ini and a shared library), puts
// them in order, loads them, and keeps what each registered — event handlers,
// services, settings, key bindings, the input capture — so it can be dropped
// when the mod is unloaded (at exit, from the Mods menu, or for a hot reload).
// Platform code: C++11 and the host's libraries; every entry point runs on the
// game thread, the log on any.
#include "mod_host.h"
#include "../../multiplayer/mp_game_api.h"

#include "port_warp.h"
#include "sms_gx/gx_pc.h"
#ifdef SMS_HAVE_GAME_BUILD_H
#include "sms_game_build.h"
#endif
#ifndef SMS_GAME_BUILD
#define SMS_GAME_BUILD "unknown"
#endif

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include <algorithm>
#include <chrono>
#include <mutex>
#include <set>

#ifdef _WIN32
#include <direct.h>
#include <windows.h>
#else
#include <dirent.h>
#include <dlfcn.h>
#endif

extern "C" void port_log(const char* fmt, ...);
// The control bindings.txt binds a key to, or null (platform/pad).
extern "C" const char* port_pad_key_control(int scancode) __attribute__((weak));
// 1 once a code mod (Super Mario Eclipse) is linked in and started.
extern "C" int sms_mod_active(void) __attribute__((weak));

namespace modhost {
std::vector<SmsMod*> g_mods;
}

using namespace modhost;

namespace {

// ------------------------------------------------------------------ small things

bool envOn(const char* name, bool def)
{
	const char* e = getenv(name);
	if (!e || !*e)
		return def;
	return strcmp(e, "0") != 0 && strcmp(e, "off") != 0 && strcmp(e, "no") != 0;
}

std::string trim(const std::string& s)
{
	size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
	return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}

std::vector<std::string> splitList(const std::string& s)
{
	std::vector<std::string> out;
	std::string cur;
	for (char c : s) {
		if (c == ',' || c == ';' || c == ' ' || c == '\t') {
			if (!cur.empty())
				out.push_back(cur);
			cur.clear();
		} else {
			cur += c;
		}
	}
	if (!cur.empty())
		out.push_back(cur);
	return out;
}

bool isDir(const std::string& p)
{
	struct stat st;
	return stat(p.c_str(), &st) == 0 && (st.st_mode & S_IFDIR);
}

bool isFile(const std::string& p, long long* stamp = nullptr)
{
	struct stat st;
	if (stat(p.c_str(), &st) != 0 || !(st.st_mode & S_IFREG))
		return false;
	if (stamp)
		*stamp = (long long)st.st_mtime * 1000000000LL
#if defined(__linux__)
		         + st.st_mtim.tv_nsec
#endif
		    ;
	return true;
}

void makeDirs(const std::string& p)
{
	for (size_t i = 1; i <= p.size(); i++)
		if (i == p.size() || p[i] == '/' || p[i] == '\\') {
			std::string part = p.substr(0, i);
#ifdef _WIN32
			_mkdir(part.c_str());
#else
			mkdir(part.c_str(), 0755);
#endif
		}
}

// key = value lines; # starts a comment line.
std::map<std::string, std::string> readIni(const std::string& path)
{
	std::map<std::string, std::string> out;
	FILE* f = fopen(path.c_str(), "r");
	if (!f)
		return out;
	char line[1024];
	while (fgets(line, sizeof line, f)) {
		std::string s = trim(line);
		if (s.empty() || s[0] == '#' || s[0] == ';' || s[0] == '[')
			continue;
		size_t eq = s.find('=');
		if (eq == std::string::npos)
			continue;
		out[trim(s.substr(0, eq))] = trim(s.substr(eq + 1));
	}
	fclose(f);
	return out;
}

bool writeIni(const std::string& path, const std::map<std::string, std::string>& kv, const char* header)
{
	const std::string tmp = path + ".tmp";
	FILE* f = fopen(tmp.c_str(), "w");
	if (!f)
		return false;
	if (header)
		fprintf(f, "# %s\n", header);
	for (const auto& e : kv)
		fprintf(f, "%s = %s\n", e.first.c_str(), e.second.c_str());
	fclose(f);
#ifdef _WIN32
	remove(path.c_str());
#endif
	return rename(tmp.c_str(), path.c_str()) == 0;
}

const char* platformDir()
{
#if defined(_WIN32)
#if defined(_WIN64)
	return "windows-x86_64";
#else
	return "windows-x86";
#endif
#elif defined(__APPLE__)
#if defined(__aarch64__)
	return "macos-arm64";
#else
	return "macos-x86_64";
#endif
#else
#if defined(__x86_64__)
	return "linux-x86_64";
#elif defined(__i386__)
	return "linux-x86";
#elif defined(__aarch64__)
	return "linux-arm64";
#else
	return "linux-unknown";
#endif
#endif
}

const char* libExt()
{
#if defined(_WIN32)
	return ".dll";
#elif defined(__APPLE__)
	return ".dylib";
#else
	return ".so";
#endif
}

void* libOpen(const std::string& path, std::string* err)
{
#ifdef _WIN32
	void* h = (void*)LoadLibraryA(path.c_str());
	if (!h) {
		char buf[64];
		snprintf(buf, sizeof buf, "LoadLibrary failed (error %lu)", (unsigned long)GetLastError());
		*err = buf;
	}
	return h;
#else
	void* h = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
	if (!h)
		*err = dlerror();
	return h;
#endif
}

void* libSym(void* h, const char* name)
{
#ifdef _WIN32
	return (void*)GetProcAddress((HMODULE)h, name);
#else
	return dlsym(h, name);
#endif
}

void libClose(void* h)
{
#ifdef _WIN32
	FreeLibrary((HMODULE)h);
#else
	dlclose(h);
#endif
}

bool copyFile(const std::string& from, const std::string& to)
{
	FILE* a = fopen(from.c_str(), "rb");
	if (!a)
		return false;
	FILE* b = fopen(to.c_str(), "wb");
	if (!b) {
		fclose(a);
		return false;
	}
	char buf[65536];
	size_t n;
	bool ok = true;
	while ((n = fread(buf, 1, sizeof buf, a)) > 0)
		ok = ok && fwrite(buf, 1, n, b) == n;
	fclose(a);
	fclose(b);
	return ok;
}

// The folders in `root`, without . and ..
std::vector<std::string> listDirs(const std::string& root)
{
	std::vector<std::string> out;
#ifdef _WIN32
	WIN32_FIND_DATAA fd;
	HANDLE h = FindFirstFileA((root + "/*").c_str(), &fd);
	if (h == INVALID_HANDLE_VALUE)
		return out;
	do
		if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && fd.cFileName[0] != '.')
			out.push_back(fd.cFileName);
	while (FindNextFileA(h, &fd));
	FindClose(h);
#else
	if (DIR* d = opendir(root.c_str())) {
		while (dirent* e = readdir(d))
			if (e->d_name[0] != '.' && isDir(root + "/" + e->d_name))
				out.push_back(e->d_name);
		closedir(d);
	}
#endif
	return out;
}

// ------------------------------------------------------------------ state

bool s_started    = false;
bool s_booting    = false; // loading the mods found at start, natives then scripts
bool s_on         = true;
bool s_hot        = false;
bool s_debug      = false;
std::string s_root;     // the mods folder
std::string s_dataRoot; // where mods' settings and data go
std::set<std::string> s_offList; // switched off in the Mods menu (native-mods.ini)
uint32_t s_frame = 0;
int s_mode       = 0;
SmsMod* s_current = nullptr;
SmsMod* s_captor  = nullptr;
int s_nextOrder   = 0;
std::vector<std::pair<SmsMod*, bool>> s_pendingOff; // menu requests, applied at the next frame
std::chrono::steady_clock::time_point s_start = std::chrono::steady_clock::now();
double s_lastSave = 0;

double now() { return std::chrono::duration<double>(std::chrono::steady_clock::now() - s_start).count(); }

// events
struct Sub {
	SmsMod* mod;
	SmsEventFn fn;
	void* user;
	int priority, handle;
	bool dead;
};
std::map<uint32_t, std::vector<Sub>> s_subs;
std::vector<std::pair<uint32_t, Sub>> s_newSubs; // added during a dispatch
int s_depth      = 0;
int s_nextHandle = 1;
int s_live[64];  // live handlers of each built-in event
std::map<std::string, uint32_t> s_customIds;
uint32_t s_nextCustom = SMS_EV_CUSTOM_BASE;
std::string s_saveStateReason;

// services
struct Service {
	SmsMod* mod;
	uint32_t version;
	const void* iface;
};
std::map<std::string, Service> s_services;

// script runtimes (core->host_runtime): the native mod running each
struct Host {
	SmsMod* mod;
	SmsScriptStartFn start;
	SmsScriptStopFn stop;
	void* user;
};
std::map<std::string, Host> s_hosts;

// input
SmsPadStatus s_raw[4];
uint16_t s_prevHeld[4], s_pressed[4], s_released[4];
uint8_t s_keyDown[512], s_keyPressed[512];
struct Binding {
	SmsMod* mod;
	std::string action;
};
std::map<int, Binding> s_bindings;
int s_menuKey = -1;

bool cancellable(uint32_t type)
{
	switch (type) {
	case SMS_EV_STAGE_EXIT_REQUEST:
	case SMS_EV_GAME_OVER:
	case SMS_EV_COINS_RESET:
	case SMS_EV_OBJECT_HIT:
	case SMS_EV_SAVE_STATE_REQUEST:
	case SMS_EV_PLAYER_STATUS:
	case SMS_EV_PLAYER_DAMAGE:
		return true;
	default:
		return type >= SMS_EV_CUSTOM_BASE;
	}
}

bool consumable(uint32_t type) { return type == SMS_EV_KEY || type == SMS_EV_HIDE_PLAYER_MODEL; }

void insertSub(uint32_t type, const Sub& s)
{
	std::vector<Sub>& v = s_subs[type];
	// by priority, then load order, then subscription order
	auto it = std::upper_bound(v.begin(), v.end(), s, [](const Sub& a, const Sub& b) {
		if (a.priority != b.priority)
			return a.priority < b.priority;
		if (a.mod->order != b.mod->order)
			return a.mod->order < b.mod->order;
		return a.handle < b.handle;
	});
	v.insert(it, s);
	if (type < 64)
		s_live[type]++;
}

// Outside every dispatch: adds what was subscribed during one, drops the dead.
void settleSubs()
{
	if (s_depth)
		return;
	for (auto& p : s_newSubs)
		if (!p.second.dead)
			insertSub(p.first, p.second);
	s_newSubs.clear();
	for (auto& e : s_subs)
		e.second.erase(std::remove_if(e.second.begin(), e.second.end(), [](const Sub& s) { return s.dead; }),
		               e.second.end());
}

void killSub(uint32_t type, Sub& s)
{
	if (s.dead)
		return;
	s.dead = true;
	if (type < 64)
		s_live[type]--;
}

void failMod(SmsMod* m, const char* why);

int callHandler(Sub& s, SmsEvent* ev)
{
	SmsMod* prev = s_current;
	s_current    = s.mod;
	int r        = SMS_CONTINUE;
	try {
		r = s.fn(s.mod, ev, s.user);
	} catch (...) {
		failMod(s.mod, "a handler threw an exception");
	}
	s_current = prev;
	return r;
}

int dispatchTo(uint32_t type, void* data, uint32_t size, SmsMod* only)
{
	auto it = s_subs.find(type);
	if (it == s_subs.end() || it->second.empty())
		return 0;
	SmsEvent ev;
	ev.type         = type;
	ev.size         = size;
	ev.data         = data;
	ev.cancelled    = 0;
	ev.cancelled_by = nullptr;
	const bool canCancel = cancellable(type), canConsume = consumable(type);
	s_depth++;
	std::vector<Sub>& v = it->second; // not reallocated during the dispatch: new handlers wait
	for (size_t i = 0; i < v.size(); i++) {
		Sub& s = v[i];
		if (s.dead || s.mod->state != SmsMod::LOADED || (only && s.mod != only))
			continue;
		const int r = callHandler(s, &ev);
		if (r != SMS_CANCEL)
			continue;
		if (canConsume) {
			ev.cancelled    = 1;
			ev.cancelled_by = s.mod->id.c_str();
			break;
		}
		if (canCancel) {
			if (!ev.cancelled) {
				ev.cancelled    = 1;
				ev.cancelled_by = s.mod->id.c_str();
			}
		} else if (s_debug) {
			log(s.mod, SMS_LOG_DEBUG, "cancelled event %u, which cannot be cancelled", (unsigned)type);
		}
	}
	s_depth--;
	settleSubs();
	return ev.cancelled;
}

// ------------------------------------------------------------------ settings

std::string dataRoot()
{
	if (!s_dataRoot.empty())
		return s_dataRoot;
	if (const char* d = getenv("SMS_NATIVE_MODS_DATA"))
		s_dataRoot = d;
	else if (const char* x = getenv("XDG_DATA_HOME"))
		s_dataRoot = std::string(x) + "/sms-port/mods";
#ifdef _WIN32
	else if (const char* x = getenv("APPDATA"))
		s_dataRoot = std::string(x) + "/sms-port/mods";
#endif
	else
		s_dataRoot = std::string(getenv("HOME") ? getenv("HOME") : ".") + "/.local/share/sms-port/mods";
	return s_dataRoot;
}

const std::string& modDataDir(SmsMod* m)
{
	if (m->dataDir.empty()) {
		m->dataDir = dataRoot() + "/" + m->id;
		makeDirs(m->dataDir);
	}
	return m->dataDir;
}

void readSettings(SmsMod* m)
{
	if (m->settingsRead)
		return;
	m->settingsRead = true;
	auto kv         = readIni(modDataDir(m) + "/settings.ini");
	for (auto& e : kv)
		m->settings.insert(e); // what the mod set before reading keeps
}

void saveSettings(SmsMod* m)
{
	if (!m->settingsDirty)
		return;
	m->settingsDirty = false;
	if (!writeIni(modDataDir(m) + "/settings.ini", m->settings, ("settings of " + m->id).c_str()))
		log(m, SMS_LOG_WARN, "could not write %s/settings.ini", m->dataDir.c_str());
}

void saveOffList()
{
	std::string list;
	for (const std::string& id : s_offList)
		list += (list.empty() ? "" : ";") + id;
	makeDirs(dataRoot());
	std::map<std::string, std::string> kv;
	kv["off"] = list;
	writeIni(dataRoot() + "/native-mods.ini", kv, "native mods switched off in the Mods menu (F1)");
}

// ------------------------------------------------------------------ finding and ordering

bool parseManifest(SmsMod* m, const std::string& path)
{
	auto kv = readIni(path);
	auto get = [&](const char* k, const char* def) {
		auto it = kv.find(k);
		return it == kv.end() ? std::string(def) : it->second;
	};
	m->id = get("id", "");
	if (m->id.empty()) {
		// the folder's name
		std::string d = m->dir;
		while (!d.empty() && (d.back() == '/' || d.back() == '\\'))
			d.pop_back();
		size_t slash = d.find_last_of("/\\");
		m->id        = slash == std::string::npos ? d : d.substr(slash + 1);
	}
	m->name        = get("name", m->id.c_str());
	m->version     = get("version", "0");
	m->author      = get("author", "");
	m->description = get("description", "");
	m->library     = get("library", m->id.c_str());
	m->after       = splitList(get("after", ""));
	m->before      = splitList(get("before", ""));
	m->requires    = splitList(get("requires", ""));
	m->conflicts   = splitList(get("conflicts", ""));
	m->priority    = atoi(get("priority", "0").c_str());
	const std::string gp = get("gameplay", "0");
	m->gameplay = gp == "1" || gp == "on" || gp == "yes" || gp == "true";
	const std::string side = get("side", "local");
	if (side != "local" && side != "server") {
		m->error = "its side \"" + side + "\" is neither local nor server";
		return false;
	}
	m->server   = side == "server";
	const std::string capi = get("client_api", "");
	int cmaj = 0, cmin = 0;
	if (!capi.empty() && sscanf(capi.c_str(), "%d.%d", &cmaj, &cmin) >= 1)
		m->clientApi = (cmaj << 8) | (cmin & 0xFF);
	m->runtime  = get("runtime", "");
	m->script   = get("script", m->runtime.empty() ? "" : ("main." + m->runtime).c_str());
	m->ini      = kv;
	for (char c : m->id)
		if (!(isalnum((unsigned char)c) || c == '-' || c == '_' || c == '.')) {
			m->error = "its id \"" + m->id + "\" may only have letters, digits, - _ and .";
			return false;
		}
	return true;
}

std::string findLibrary(SmsMod* m, std::string* looked)
{
	const std::string file = m->library + libExt();
	const std::string tries[] = {
		m->dir + "/bin/" + platformDir() + "/" + file,
		m->dir + "/" + platformDir() + "/" + file,
		m->dir + "/" + file,
	};
	for (const std::string& t : tries) {
		if (isFile(t))
			return t;
		*looked += (looked->empty() ? "" : ", ") + t;
	}
	return std::string();
}

SmsMod* byId(const std::string& id)
{
	for (SmsMod* m : g_mods)
		if (m->id == id)
			return m;
	return nullptr;
}

void discover()
{
	std::vector<std::string> roots;
	if (const char* d = getenv("SMS_NATIVE_MODS_DIR")) {
		// folders, split at ';' only: their paths may have spaces ("SMS Launcher")
		std::string cur;
		for (const char* c = d;; c++) {
			if (*c == ';' || !*c) {
				while (!cur.empty() && (cur.back() == ' ' || cur.back() == '\t'))
					cur.pop_back();
				if (!cur.empty())
					roots.push_back(cur);
				cur.clear();
				if (!*c)
					break;
			} else if (!cur.empty() || (*c != ' ' && *c != '\t')) {
				cur += *c;
			}
		}
	}
	else
		for (const char* r : {"mods", "../../mods"})
			if (isDir(r)) {
				roots.push_back(r);
				break;
			}
	std::vector<SmsMod*> found;
	for (const std::string& root : roots) {
		if (s_root.empty())
			s_root = root;
		std::vector<std::string> names = listDirs(root);
		std::sort(names.begin(), names.end());
		for (const std::string& n : names) {
			const std::string dir = root + "/" + n;
			if (!isFile(dir + "/mod.ini"))
				continue;
			SmsMod* m = new SmsMod;
			m->dir    = dir;
			if (!parseManifest(m, dir + "/mod.ini")) {
				m->state = SmsMod::FAILED;
			} else if (byId(m->id)) {
				m->state = SmsMod::FAILED;
				m->error = "another mod has the id \"" + m->id + "\" (" + byId(m->id)->dir + ")";
			}
			g_mods.push_back(m);
		}
	}
}

// Load order: `requires` and `after` load first, `before` later; otherwise by
// priority, then id. A cycle keeps the remaining mods in that order.
void order()
{
	std::vector<SmsMod*> todo;
	for (SmsMod* m : g_mods)
		if (m->state == SmsMod::FOUND)
			todo.push_back(m);
	std::sort(todo.begin(), todo.end(), [](SmsMod* a, SmsMod* b) {
		return a->priority != b->priority ? a->priority < b->priority : a->id < b->id;
	});
	auto mustPrecede = [&](SmsMod* a, SmsMod* b) { // a before b
		auto has = [](const std::vector<std::string>& l, const std::string& s) {
			return std::find(l.begin(), l.end(), s) != l.end();
		};
		return has(b->after, a->id) || has(b->requires, a->id) || has(a->before, b->id);
	};
	std::vector<SmsMod*> out;
	while (!todo.empty()) {
		size_t pick = todo.size();
		for (size_t i = 0; i < todo.size() && pick == todo.size(); i++) {
			bool ready = true;
			for (size_t j = 0; j < todo.size() && ready; j++)
				if (j != i && mustPrecede(todo[j], todo[i]))
					ready = false;
			if (ready)
				pick = i;
		}
		if (pick == todo.size()) {
			std::string ids;
			for (SmsMod* m : todo)
				ids += " " + m->id;
			log(nullptr, SMS_LOG_WARN, "load order has a cycle among:%s; loading them by priority", ids.c_str());
			pick = 0;
		}
		out.push_back(todo[pick]);
		todo.erase(todo.begin() + (long)pick);
	}
	std::vector<SmsMod*> rest;
	for (SmsMod* m : g_mods)
		if (std::find(out.begin(), out.end(), m) == out.end())
			rest.push_back(m);
	for (SmsMod* m : out)
		m->order = s_nextOrder++;
	out.insert(out.end(), rest.begin(), rest.end());
	g_mods = out;
}

// ------------------------------------------------------------------ dropping what a mod registered

void dropRegistrations(SmsMod* m)
{
	for (auto& e : s_subs)
		for (Sub& s : e.second)
			if (s.mod == m)
				killSub(e.first, s);
	for (auto& p : s_newSubs)
		if (p.second.mod == m)
			p.second.dead = true;
	settleSubs();
	for (auto it = s_services.begin(); it != s_services.end();)
		it = it->second.mod == m ? s_services.erase(it) : std::next(it);
	for (auto it = s_hosts.begin(); it != s_hosts.end();)
		it = it->second.mod == m ? s_hosts.erase(it) : std::next(it);
	for (auto it = s_bindings.begin(); it != s_bindings.end();)
		it = it->second.mod == m ? s_bindings.erase(it) : std::next(it);
	if (s_captor == m)
		s_captor = nullptr;
	m->options.clear();
	drawDropMod(m);
}

void failMod(SmsMod* m, const char* why)
{
	log(m, SMS_LOG_ERROR, "switched off: %s", why);
	m->error = why;
	m->state = SmsMod::FAILED;
	dropRegistrations(m); // its library stays loaded: its code may still be on the stack
}

// ------------------------------------------------------------------ the API: core

void apiLog(SmsMod* mod, int level, const char* fmt, ...) SMS_MOD_PRINTF(3, 4);
void apiLog(SmsMod* mod, int level, const char* fmt, ...)
{
	if (level <= SMS_LOG_DEBUG && !s_debug)
		return;
	char buf[2048];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof buf, fmt, ap);
	va_end(ap);
	log(mod, level, "%s", buf);
}

int apiSubscribe(SmsMod* mod, uint32_t type, SmsEventFn fn, void* user, int priority)
{
	if (!mod || !fn || mod->state != SmsMod::LOADED)
		return 0;
	Sub s = {mod, fn, user, priority, s_nextHandle++, false};
	if (s_depth) {
		s_newSubs.push_back(std::make_pair(type, s));
	} else {
		insertSub(type, s);
	}
	// mods that loaded before this handler
	if (type == SMS_EV_MOD_LOADED)
		for (SmsMod* o : g_mods)
			if (o != mod && o->state == SmsMod::LOADED) {
				SmsEvMod p = {o->id.c_str(), o->name.c_str(), o->version.c_str()};
				SmsEvent ev = {type, (uint32_t)sizeof p, &p, 0, nullptr};
				callHandler(s, &ev);
			}
	return s.handle;
}

void apiUnsubscribe(SmsMod* mod, int handle)
{
	for (auto& e : s_subs)
		for (Sub& s : e.second)
			if (s.handle == handle && s.mod == mod)
				killSub(e.first, s);
	for (auto& p : s_newSubs)
		if (p.second.handle == handle && p.second.mod == mod)
			p.second.dead = true;
	settleSubs();
}

uint32_t apiEventId(const char* name)
{
	if (!name || !*name)
		return 0;
	auto it = s_customIds.find(name);
	if (it != s_customIds.end())
		return it->second;
	return s_customIds[name] = s_nextCustom++;
}

int apiEmit(SmsMod*, uint32_t type, void* data, uint32_t size)
{
	if (type < SMS_EV_CUSTOM_BASE)
		return 0; // the game's events are the game's to send
	return dispatchTo(type, data, size, nullptr);
}

int apiProvide(SmsMod* mod, const char* name, uint32_t version, const void* iface)
{
	if (!mod || !name || !iface)
		return 0;
	auto it = s_services.find(name);
	if (it != s_services.end() && it->second.mod != mod) {
		log(mod, SMS_LOG_WARN, "service \"%s\" is already provided by %s; keeping theirs", name,
		    it->second.mod->id.c_str());
		return 0;
	}
	s_services[name] = Service{mod, version, iface};
	return 1;
}

const void* apiRequire(SmsMod* mod, const char* name, uint32_t minVersion)
{
	auto it = name ? s_services.find(name) : s_services.end();
	if (it == s_services.end() || it->second.version < minVersion) {
		log(mod, SMS_LOG_DEBUG, "service \"%s\" v%u not available", name ? name : "", (unsigned)minVersion);
		return nullptr;
	}
	return it->second.iface;
}

const char* apiModId(SmsMod* m) { return m ? m->id.c_str() : ""; }
const char* apiModDir(SmsMod* m) { return m ? m->dir.c_str() : ""; }
const char* apiDataDir(SmsMod* m) { return m ? modDataDir(m).c_str() : ""; }
int apiIsLoaded(const char* id)
{
	SmsMod* m = id ? byId(id) : nullptr;
	return m && m->state == SmsMod::LOADED;
}
const char* apiGameBuild(void) { return SMS_GAME_BUILD; }
int apiPointerSize(void) { return (int)sizeof(void*); }
double apiTime(void) { return now(); }

const char* apiModInfo(SmsMod* m, const char* key)
{
	if (!m || !key)
		return nullptr;
	auto it = m->ini.find(key);
	return it == m->ini.end() ? nullptr : it->second.c_str();
}

int apiHostRuntime(SmsMod* host, const char* runtime, SmsScriptStartFn start, SmsScriptStopFn stop, void* user)
{
	if (!host || !runtime || !*runtime || !start || !stop || host->state != SmsMod::LOADED || !host->runtime.empty())
		return 0;
	auto it = s_hosts.find(runtime);
	if (it != s_hosts.end() && it->second.mod != host) {
		log(host, SMS_LOG_WARN, "%s scripts are already run by %s; keeping theirs", runtime,
		    it->second.mod->id.c_str());
		return 0;
	}
	s_hosts[runtime] = Host{host, start, stop, user};
	log(host, SMS_LOG_INFO, "runs %s script mods", runtime);
	return 1;
}

const SmsCoreApi s_core = {
	sizeof(SmsCoreApi), apiLog,     apiSubscribe, apiUnsubscribe, apiEventId,    apiEmit,         apiProvide,
	apiRequire,         apiModId,   apiModDir,    apiDataDir,     apiIsLoaded,   apiGameBuild,    apiPointerSize,
	apiTime,            apiModInfo, apiHostRuntime,
};

// ------------------------------------------------------------------ the API: game, player, camera, audio, native

void apiState(SmsGameState* out)
{
	if (!out)
		return;
	if (!out->size)
		out->size = sizeof *out;
	modg_state(out);
	SmsGameState full;
	memset(&full, 0, sizeof full);
	if (out->size >= offsetof(SmsGameState, frame) + sizeof full.frame)
		out->frame = s_frame;
	if (out->size >= offsetof(SmsGameState, eclipse) + sizeof full.eclipse)
		out->eclipse = sms_mod_active && sms_mod_active();
}

int apiCoins(void) { return modg_flag_get(0x40002); }
int apiLives(void) { return modg_flag_get(0x20001); }
int apiShines(void) { return modg_flag_get(0x40000); }
int apiBlueCoins(void) { return modg_flag_get(0x40001); }
int apiHasShine(int shine) { return shine >= 0 && shine < 120 && modg_flag_get(0x10000 + (uint32_t)shine) != 0; }

// The stage names the warp menu's list has (asked for once, filled by the game).
std::map<int, std::string> s_areaNames;
const char* apiAreaName(int area)
{
	if (s_areaNames.empty()) {
		const int n = port_warp_count();
		for (int i = 0; i < n; i++) {
			PortWarpEntry e;
			if (port_warp_entry(i, &e) && !s_areaNames.count(e.area))
				s_areaNames[e.area] = e.world;
		}
	}
	auto it = s_areaNames.find(area);
	return it == s_areaNames.end() ? "" : it->second.c_str();
}

int apiInCube(int set, int index, const SmsVec3* p) { return p ? modg_in_cube(set, index, p->x, p->y, p->z) : -1; }

int apiObjectCount(uint32_t lo, uint32_t hi, const char* kind) { return mpg_objects(lo, hi, kind, -1, nullptr); }

int apiObjectInfo(uint32_t lo, uint32_t hi, const char* kind, int index, SmsObjectInfo* out)
{
	MpgObjectInfo o;
	if (!out || index < 0 || !mpg_objects(lo, hi, kind, index, &o))
		return 0;
	SmsObjectInfo s;
	memset(&s, 0, sizeof s);
	s.size = sizeof s;
	s.type = o.type;
	snprintf(s.kind, sizeof s.kind, "%s", o.kind);
	s.position = {o.position[0], o.position[1], o.position[2]};
	s.rotation = {o.rotation[0], o.rotation[1], o.rotation[2]};
	s.scale    = {o.scale[0], o.scale[1], o.scale[2]};
	s.alive    = o.alive;
	s.hidden   = o.hidden;
	s.spawned  = o.spawned;
	const uint32_t n = out->size && out->size < sizeof s ? out->size : (uint32_t)sizeof s;
	memcpy(out, &s, n);
	out->size = n;
	return 1;
}

int apiGround(const SmsVec3* p, SmsGround* out)
{
	MpgGround g;
	if (!p || !out || !mpg_ground(p->x, p->y, p->z, &g))
		return 0;
	SmsGround s;
	memset(&s, 0, sizeof s);
	s.size       = sizeof s;
	s.floor      = g.floor;
	s.floor_type = g.floorType;
	s.water      = g.water;
	s.has_water  = g.hasWater;
	s.water_y    = g.waterY;
	s.roof       = g.roof;
	const uint32_t n = out->size && out->size < sizeof s ? out->size : (uint32_t)sizeof s;
	memcpy(out, &s, n);
	out->size = n;
	return 1;
}

const SmsGameApi s_game = {
	sizeof(SmsGameApi), apiState,        modg_flag_get,       modg_flag_set,         apiCoins,
	apiLives,           apiShines,       apiBlueCoins,        apiHasShine,           modg_warp,
	modg_find_object,   modg_object_name, modg_object_position, modg_object_set_position, apiAreaName,
	modg_cube_count,    apiInCube,       apiObjectCount,      apiObjectInfo,         apiGround,
};

int apiPlayerGet(SmsPlayer* out)
{
	if (!out)
		return 0;
	if (!out->size)
		out->size = sizeof *out;
	return modg_player_get(out);
}

const SmsPlayerApi s_player = {
	sizeof(SmsPlayerApi),          apiPlayerGet,          modg_player_set_position, modg_player_set_velocity,
	modg_player_set_forward_speed, modg_player_set_health, modg_player_set_water,   modg_player_set_status,
};

// This frame's camera, read once a frame for the projections.
SmsCamera s_cam;
uint32_t s_camFrame = ~0u;

const SmsCamera& camera()
{
	if (s_camFrame != s_frame) {
		s_camFrame = s_frame;
		s_cam.size = sizeof s_cam;
		modg_camera_get(&s_cam);
	}
	return s_cam;
}

int apiCameraGet(SmsCamera* out)
{
	if (!out)
		return 0;
	const uint32_t n = out->size && out->size < sizeof s_cam ? out->size : (uint32_t)sizeof s_cam;
	memcpy(out, &camera(), n);
	out->size = n;
	return camera().valid;
}

int apiWorldToCanvas(const SmsVec3* p, float* x, float* y, float* depth)
{
	const SmsCamera& c = camera();
	if (!p || !c.valid)
		return 0;
	float v[3];
	for (int r = 0; r < 3; r++)
		v[r] = c.view[r][0] * p->x + c.view[r][1] * p->y + c.view[r][2] * p->z + c.view[r][3];
	float clip[4];
	for (int r = 0; r < 4; r++)
		clip[r] = c.proj[r][0] * v[0] + c.proj[r][1] * v[1] + c.proj[r][2] * v[2] + c.proj[r][3];
	if (depth)
		*depth = -v[2];
	if (clip[3] <= 1e-4f)
		return 0;
	float w, h;
	drawCanvas(&w, &h);
	if (x)
		*x = (clip[0] / clip[3] + 1.0f) * 0.5f * w;
	if (y)
		*y = (1.0f - clip[1] / clip[3]) * 0.5f * h;
	return 1;
}

const SmsCameraApi s_camera = {sizeof(SmsCameraApi), apiCameraGet, apiWorldToCanvas};

const SmsAudioApi s_audio = {sizeof(SmsAudioApi), modg_play_system_sound};

const char* apiNativeBuild(void)
{
	static std::string b = std::string(SMS_GAME_BUILD) + "/" + platformDir();
	return b.c_str();
}

const SmsNativeApi s_native = {sizeof(SmsNativeApi), apiNativeBuild, modg_native};

// ------------------------------------------------------------------ the API: multiplayer

// Each character's colour, for text about them (SMS_NET_CHAR_*).
const SmsColor kCharColor[SMS_NET_MAX_PLAYERS] = {
	SMS_RGB(255, 64, 64),  SMS_RGB(64, 208, 96),  SMS_RGB(255, 208, 32), SMS_RGB(200, 200, 216),
	SMS_RGB(255, 255, 255), SMS_RGB(192, 112, 255), SMS_RGB(255, 154, 48), SMS_RGB(128, 216, 255),
};

int apiNetRole(void) { return mpg_role(); }
int apiNetDedicated(void) { return mpg_dedicated(); }
int apiNetLocalSlot(void) { return mpg_slot(); }

uint32_t apiNetPlayers(void)
{
	uint32_t bits = 0;
	for (int i = 0; i < SMS_NET_MAX_PLAYERS; i++)
		if (mpg_player_present(i))
			bits |= 1u << i;
	return bits;
}

int apiNetPlayer(int slot, SmsNetPlayer* out)
{
	if (!out || slot < 0 || slot >= SMS_NET_MAX_PLAYERS || !mpg_player_present(slot))
		return 0;
	SmsNetPlayer p;
	memset(&p, 0, sizeof p);
	p.size      = sizeof p;
	p.slot      = slot;
	p.server    = mpg_dedicated() && slot == 0;
	p.character = mpg_character(slot);
	mpg_player_name(slot, p.name, sizeof p.name);
	if (!p.name[0])
		snprintf(p.name, sizeof p.name, "%s", mpg_character_name(slot));
	p.color = kCharColor[p.character & 7];
	p.ping  = slot == mpg_slot() ? 0 : mpg_ping(mpg_role() == MPG_ROLE_HOST ? slot : 0);
	int api = 0;
	mpg_player_version(slot, p.game_version, sizeof p.game_version, &api);
	p.api_version = (uint32_t)(api >> 8) << 16 | (uint32_t)(api & 0xFF);
	if (slot == mpg_slot()) {
		SmsPlayer me;
		memset(&me, 0, sizeof me);
		me.size = sizeof me;
		if (modg_player_get(&me)) {
			p.position = me.position;
			p.yaw      = me.yaw;
			p.flags    = (p.server ? 0 : SMS_NETP_VISIBLE) | (me.on_yoshi ? SMS_NETP_YOSHI : 0);
		}
		p.in_stage = mpg_role() == MPG_ROLE_HOST ? mpg_player_arrived(slot) : mpg_session() != 0;
		int8_t stick[4];
		uint8_t trigger[2];
		mpg_local_pad(&p.pad.buttons, stick, trigger);
		p.pad.stick_x = stick[0], p.pad.stick_y = stick[1], p.pad.cstick_x = stick[2], p.pad.cstick_y = stick[3];
		p.pad.trigger_l = trigger[0], p.pad.trigger_r = trigger[1];
	} else {
		MpgPose pose;
		float age = 0;
		if (mpg_pose(slot, &pose, &age)) {
			p.position.x = pose.mtx[0][3];
			p.position.y = pose.mtx[1][3];
			p.position.z = pose.mtx[2][3];
			p.yaw        = atan2f(pose.mtx[0][2], pose.mtx[2][2]) * (180.0f / 3.14159265f);
			p.flags = (pose.flags & MPG_POSE_VISIBLE ? SMS_NETP_VISIBLE : 0) | (pose.more & MPG_POSE2_SURF ? SMS_NETP_SURFING : 0)
			          | (pose.flags & MPG_POSE_YOSHI ? SMS_NETP_YOSHI : 0)
			          | (pose.flags & MPG_POSE_SPECTATING ? SMS_NETP_SPECTATING : 0)
			          | (pose.more & MPG_POSE2_PAUSED ? SMS_NETP_PAUSED : 0) | (pose.flags & MPG_POSE_DEMO ? SMS_NETP_CUTSCENE : 0);
			if (pose.hasPad) {
				p.pad.buttons = pose.padButtons;
				p.pad.stick_x = pose.padStick[0], p.pad.stick_y = pose.padStick[1];
				p.pad.cstick_x = pose.padStick[2], p.pad.cstick_y = pose.padStick[3];
				p.pad.trigger_l = pose.padTrigger[0], p.pad.trigger_r = pose.padTrigger[1];
			} else {
				p.pad.err = -1; // an older game says nothing of it
			}
		}
		p.in_stage = mpg_role() == MPG_ROLE_HOST ? mpg_player_arrived(slot) : pose.session == mpg_session() && pose.session;
	}
	const uint32_t n = out->size && out->size < sizeof p ? out->size : (uint32_t)sizeof p;
	memcpy(out, &p, n);
	out->size = n;
	return 1;
}

uint32_t apiNetSession(void) { return mpg_session(); }

uint32_t apiNetSend(uint32_t to, const SmsCmd* cmd)
{
	if (!cmd || mpg_role() != MPG_ROLE_HOST)
		return 0;
	MpgCmd c;
	memset(&c, 0, sizeof c);
	c.type    = (uint8_t)(cmd->type <= MPG_CMD_LAST ? cmd->type : 0);
	c.flags   = (uint8_t)((cmd->flags & SMS_CMD_STICKY ? MPG_CMD_STICKY : 0) | (cmd->flags & SMS_CMD_UNSTICK ? MPG_CMD_UNSTICK : 0));
	c.delayMs = cmd->delay_ms;
	memcpy(c.i, cmd->i, sizeof c.i);
	memcpy(c.f, cmd->f, sizeof cmd->f);
	if (cmd->size >= offsetof(SmsCmd, f2) + sizeof cmd->f2) // API 1.4's four more
		memcpy(c.f + 4, cmd->f2, sizeof cmd->f2);
	if (cmd->text)
		snprintf(c.text, sizeof c.text, "%s", cmd->text);
	if (!c.type) {
		log(current(), SMS_LOG_WARN, "net->send: no command of type %u", (unsigned)cmd->type);
		return 0;
	}
	return mpg_server_send(to, &c);
}

int apiNetLoadStage(int area, int episode) { return mpg_server_load_stage(area, episode); }
int apiNetSetLook(int slot, const char* look) { return mpg_role() == MPG_ROLE_HOST && mpg_server_set_look(slot, look); }
int apiNetLook(int slot, char* out, int size) { return out && size > 0 ? mpg_look(slot, out, size) : 0; }
int apiNetSetRespawn(int slot, int how, const SmsVec3* p, float yaw)
{
	return mpg_role() == MPG_ROLE_HOST && mpg_server_set_respawn(slot, p ? how : 0, p ? p->x : 0, p ? p->y : 0, p ? p->z : 0, yaw);
}
void apiNetSetMode(const char* name) { mpg_server_mode(name); }
void apiNetSetStatus(const char* text) { mpg_server_status(text); }

const SmsNetApi s_netTable = {
	sizeof(SmsNetApi), apiNetRole, apiNetDedicated, apiNetLocalSlot, apiNetPlayers,  apiNetPlayer,
	apiNetSession,     apiNetSend, apiNetLoadStage, apiNetSetMode,   apiNetSetStatus,
	apiNetSetLook,     apiNetLook, apiNetSetRespawn,
};

// ------------------------------------------------------------------ world (API 1.4)
//
// The vanilla game's map of its stages (the game half's tables). Eclipse
// numbers its stages its own way: nothing there.

bool vanilla() { return !(sms_mod_active && sms_mod_active()); }

int apiWorldAreaKind(int area) { return vanilla() ? modg_world_area_kind(area) : SMS_AREA_NONE; }
int apiWorldOf(int area) { return vanilla() ? modg_world_of(area) : -1; }
int apiWorldEpisodeCount(int area) { return vanilla() ? modg_world_episode_count(area) : 0; }
int apiWorldEpisodeShine(int area, int episode) { return vanilla() ? modg_world_episode_shine(area, episode) : -1; }
int apiWorldEpisodeName(int area, int episode, char* out, int size)
{
	if (out && size > 0)
		out[0] = 0;
	return vanilla() ? modg_world_episode_name(area, episode, out, size) : 0;
}
int apiWorldNextEpisode(int area) { return vanilla() ? modg_world_next_episode(area) : -1; }
int apiWorldShineEpisode(int shine, int* area)
{
	if (area)
		*area = -1;
	return vanilla() ? modg_world_shine_episode(shine, area) : -1;
}
int apiWorldHasCutscene(const char* name) { return modg_world_has_cutscene(name); }
int apiWorldPlayCutscene(const char* name, const SmsVec3* focus, int frames)
{
	return modg_world_play_cutscene(name, focus, frames);
}
int apiWorldCutscenePlaying(void) { return modg_world_cutscene_playing(); }

const SmsWorldApi s_world = {
	sizeof(SmsWorldApi),  apiWorldAreaKind,     apiWorldOf,          apiWorldEpisodeCount,
	apiWorldEpisodeShine, apiWorldEpisodeName,  apiWorldNextEpisode, apiWorldShineEpisode,
	apiWorldHasCutscene,  apiWorldPlayCutscene, apiWorldCutscenePlaying,
};

// What the multiplayer game says happened, to every mod.
void netEvents()
{
	MpgNetEvent e;
	while (mpg_next_net_event(&e)) {
		if (e.type == MPG_NET_SESSION) {
			SmsEvNetSession ev = {e.area, e.episode, e.session};
			modh_dispatch(SMS_EV_NET_SESSION, &ev, sizeof ev);
			continue;
		}
		static const uint32_t kType[] = {0, SMS_EV_NET_PLAYER_JOINED, SMS_EV_NET_PLAYER_LEFT, SMS_EV_NET_PLAYER_ARRIVED};
		if (e.type < 1 || e.type > 3)
			continue;
		SmsEvNetPlayer ev = {e.slot, e.name[0] ? e.name : mpg_character_name(e.slot), mpg_character(e.slot)};
		modh_dispatch(kType[e.type], &ev, sizeof ev);
	}
}

// ------------------------------------------------------------------ the API: input

int apiPad(int port, SmsPad* out)
{
	if (!out || port < 0 || port > 3)
		return 0;
	SmsPad p;
	memset(&p, 0, sizeof p);
	p.size              = sizeof p;
	const SmsPadStatus& r = s_raw[port];
	p.connected         = r.err == 0;
	if (p.connected) {
		p.held      = r.buttons;
		p.pressed   = s_pressed[port];
		p.released  = s_released[port];
		p.stick_x   = r.stick_x / 127.0f;
		p.stick_y   = r.stick_y / 127.0f;
		p.cstick_x  = r.cstick_x / 127.0f;
		p.cstick_y  = r.cstick_y / 127.0f;
		p.trigger_l = r.trigger_l / 255.0f;
		p.trigger_r = r.trigger_r / 255.0f;
	}
	const uint32_t n = out->size && out->size < sizeof p ? out->size : (uint32_t)sizeof p;
	memcpy(out, &p, n);
	out->size = n;
	return p.connected;
}

int apiKeyDown(int sc) { return sc > 0 && sc < 512 && s_keyDown[sc]; }
int apiKeyPressed(int sc) { return sc > 0 && sc < 512 && s_keyPressed[sc]; }
int apiKeyFromName(const char* name) { return GXPC_KeyFromName(name); }
const char* apiKeyName(int sc) { return GXPC_KeyName(sc); }

// Keys the port keeps for itself (gx_platform.cpp, pad.cpp), whatever bindings.txt says.
const char* portKey(int sc)
{
	static const struct {
		const char* key;
		const char* what;
	} kPort[] = {
		{"`", "the debug overlay"},     {"F7", "the game speed (with the overlay)"}, {"F8", "the warp menu"},
		{"F9", "the cheat menu"},       {"F10", "the mouse camera"},                 {"F11", "fullscreen"},
		{"Escape", "quit"},
	};
	if (sc == s_menuKey)
		return "the Mods menu";
	for (const auto& k : kPort)
		if (GXPC_KeyFromName(k.key) == sc)
			return k.what;
	return port_pad_key_control ? port_pad_key_control(sc) : nullptr;
}

int apiBindKey(SmsMod* mod, const char* action, const char* defaultKey)
{
	if (!mod || !action)
		return -1;
	const std::string key = settingGet(mod, std::string("key.") + action, defaultKey ? defaultKey : "");
	const int sc          = GXPC_KeyFromName(key.c_str());
	if (sc < 0) {
		if (!key.empty())
			log(mod, SMS_LOG_WARN, "key \"%s\" for %s is not a key name", key.c_str(), action);
		return -1;
	}
	if (const char* what = portKey(sc))
		log(mod, SMS_LOG_WARN, "key %s for %s is also the port's (%s); set key.%s in %s/settings.ini", key.c_str(),
		    action, what, action, modDataDir(mod).c_str());
	auto it = s_bindings.find(sc);
	if (it != s_bindings.end() && !(it->second.mod == mod && it->second.action == action))
		log(mod, SMS_LOG_WARN, "key %s for %s is also %s's (%s); set key.%s in %s/settings.ini", key.c_str(), action,
		    it->second.mod->id.c_str(), it->second.action.c_str(), action, modDataDir(mod).c_str());
	else
		s_bindings[sc] = Binding{mod, action};
	return sc;
}

int apiCapture(SmsMod* mod, int on)
{
	if (!mod)
		return 0;
	if (on) {
		if (s_captor && s_captor != mod)
			return 0;
		s_captor = mod;
		return 1;
	}
	if (s_captor == mod)
		s_captor = nullptr;
	return 1;
}

const char* apiCapturedBy(void) { return s_captor ? s_captor->id.c_str() : nullptr; }

const SmsInputApi s_input = {
	sizeof(SmsInputApi), apiPad,     apiKeyDown, apiKeyPressed, apiKeyFromName,
	apiKeyName,          apiBindKey, apiCapture, apiCapturedBy,
};

// ------------------------------------------------------------------ the API: config

int apiGetInt(SmsMod* m, const char* key, int def)
{
	bool found;
	std::string v = settingGet(m, key ? key : "", "", &found);
	return found && !v.empty() ? (int)strtol(v.c_str(), nullptr, 0) : def;
}

float apiGetFloat(SmsMod* m, const char* key, float def)
{
	bool found;
	std::string v = settingGet(m, key ? key : "", "", &found);
	return found && !v.empty() ? (float)strtod(v.c_str(), nullptr) : def;
}

const char* apiGetString(SmsMod* m, const char* key, const char* def)
{
	if (!m || !key)
		return def;
	readSettings(m);
	auto it = m->settings.find(key);
	return it == m->settings.end() ? def : it->second.c_str();
}

void apiSetInt(SmsMod* m, const char* key, int v) { settingSet(m, key ? key : "", std::to_string(v)); }
void apiSetFloat(SmsMod* m, const char* key, float v)
{
	char buf[32];
	snprintf(buf, sizeof buf, "%g", v);
	settingSet(m, key ? key : "", buf);
}
void apiSetString(SmsMod* m, const char* key, const char* v) { settingSet(m, key ? key : "", v ? v : ""); }
void apiSave(SmsMod* m)
{
	if (m)
		saveSettings(m);
}

int apiAddOption(SmsMod* m, const SmsOption* o)
{
	if (!m || !o || !o->key || o->size < offsetof(SmsOption, choices))
		return 0;
	ModOption opt;
	memset(&opt.desc, 0, sizeof opt.desc);
	memcpy(&opt.desc, o, std::min<size_t>(o->size, sizeof opt.desc));
	opt.desc.size = sizeof opt.desc;
	opt.key       = o->key;
	opt.label     = o->label ? o->label : o->key;
	if (opt.desc.type == SMS_OPTION_CHOICE && opt.desc.choices)
		for (int i = 0; i < opt.desc.choice_count; i++)
			opt.choices.push_back(opt.desc.choices[i] ? opt.desc.choices[i] : "");
	for (auto it = m->options.begin(); it != m->options.end(); ++it)
		if (it->key == opt.key) {
			m->options.erase(it);
			break;
		}
	m->options.push_back(opt);
	// the strings point into the copy
	for (ModOption& mo : m->options) {
		mo.desc.key   = mo.key.c_str();
		mo.desc.label = mo.label.c_str();
		mo.choicePtrs.clear();
		for (const std::string& c : mo.choices)
			mo.choicePtrs.push_back(c.c_str());
		mo.desc.choices = mo.choicePtrs.empty() ? nullptr : mo.choicePtrs.data();
	}
	return 1;
}

const SmsConfigApi s_config = {
	sizeof(SmsConfigApi), apiGetInt, apiGetFloat, apiGetString, apiSetInt,
	apiSetFloat,          apiSetString, apiSave,  apiAddOption,
};

// ------------------------------------------------------------------ keys

int keyHook(int sc, int down, int repeat, int mods)
{
	if (sc > 0 && sc < 512) {
		s_keyDown[sc] = (uint8_t)down;
		if (down && !repeat)
			s_keyPressed[sc] = 1;
	}
	// the Mods menu's key opens and closes it, and is kept from the game
	if (sc == s_menuKey && s_menuKey >= 0) {
		if (down && !repeat)
			menuToggle();
		return 1;
	}
	if (menuOpen()) {
		menuKey(sc, down, repeat); // the menu has the keyboard while open
		return 1;
	}
	SmsEvKey ev = {sc, down, repeat, mods};
	if (s_captor) {
		dispatchTo(SMS_EV_KEY, &ev, sizeof ev, s_captor);
		return 1;
	}
	return dispatchTo(SMS_EV_KEY, &ev, sizeof ev, nullptr);
}

// ------------------------------------------------------------------ start and stop

void shutdown()
{
	if (!s_started)
		return;
	modh_dispatch(SMS_EV_SHUTDOWN, nullptr, 0);
	for (auto it = g_mods.rbegin(); it != g_mods.rend(); ++it)
		if ((*it)->state == SmsMod::LOADED)
			unloadMod(*it);
	for (SmsMod* m : g_mods)
		saveSettings(m);
}

// Loads a mod found, after checking what it needs and what it clashes with.
void tryLoad(SmsMod* m)
{
	std::string missing;
	for (const std::string& r : m->requires) {
		SmsMod* o = byId(r);
		if (!o || o->state != SmsMod::LOADED)
			missing += (missing.empty() ? "" : ", ") + r;
	}
	std::string clash;
	for (SmsMod* o : g_mods)
		if (o != m && o->state == SmsMod::LOADED &&
		    (std::find(m->conflicts.begin(), m->conflicts.end(), o->id) != m->conflicts.end() ||
		     std::find(o->conflicts.begin(), o->conflicts.end(), m->id) != o->conflicts.end()))
			clash += (clash.empty() ? "" : ", ") + o->id;
	if (!missing.empty()) {
		m->state = SmsMod::FAILED;
		m->error = "needs " + missing;
	} else if (!clash.empty()) {
		m->state = SmsMod::FAILED;
		m->error = "does not work with " + clash;
	} else if (!loadMod(m)) {
		m->state = SmsMod::FAILED;
	}
	if (m->state == SmsMod::FAILED)
		log(m, SMS_LOG_ERROR, "not loaded: %s", m->error.c_str());
}

// Script mods waiting for their runtime's host, once it is loaded again.
void startWaitingScripts()
{
	for (SmsMod* m : g_mods)
		if (!m->runtime.empty() && s_hosts.count(m->runtime) && !s_offList.count(m->id) &&
		    (m->state == SmsMod::FOUND || (m->state == SmsMod::FAILED && m->error.rfind("needs a mod", 0) == 0))) {
			m->state = SmsMod::FOUND;
			tryLoad(m);
		}
}

void start()
{
	s_started = true;
	s_on      = envOn("SMS_NATIVE_MODS", true);
	s_hot     = envOn("SMS_NATIVE_MODS_HOT", false);
	s_debug   = envOn("SMS_NATIVE_MODS_DEBUG", false);
	if (!s_on) {
		port_log("[mods] native mods switched off (SMS_NATIVE_MODS=0)\n");
		return;
	}
	discover();
	if (g_mods.empty())
		return;
	for (const std::string& id : splitList(readIni(dataRoot() + "/native-mods.ini")["off"]))
		s_offList.insert(id);
	if (const char* e = getenv("SMS_NATIVE_MODS_DISABLED"))
		for (const std::string& id : splitList(e))
			if (SmsMod* m = byId(id))
				if (m->state == SmsMod::FOUND) {
					m->state = SmsMod::OFF;
					m->error = "switched off (SMS_NATIVE_MODS_DISABLED)";
				}
	for (SmsMod* m : g_mods)
		if (m->state == SmsMod::FOUND && s_offList.count(m->id)) {
			m->state = SmsMod::OFF;
			m->error = "switched off in the Mods menu";
		}
	// a server mod runs where multiplayer is hosted; the players need nothing
	for (SmsMod* m : g_mods)
		if (m->state == SmsMod::FOUND && m->server && mpg_role() != MPG_ROLE_HOST) {
			m->state = SmsMod::OFF;
			m->error = "a server mod: it runs when this game hosts multiplayer";
		}
	order();
	port_log("[mods] API %d.%d: %zu native mod(s) in %s (%s); settings in %s\n", SMS_MOD_API_MAJOR, SMS_MOD_API_MINOR,
	         g_mods.size(), s_root.c_str(), platformDir(), dataRoot().c_str());
	// native mods first, then the script mods their runtimes run
	s_booting = true;
	for (int scripts = 0; scripts < 2; scripts++)
		for (SmsMod* m : g_mods) {
			if (m->runtime.empty() == (scripts == 1))
				continue;
			if (m->state != SmsMod::FOUND) {
				if (m->state == SmsMod::FAILED)
					log(m, SMS_LOG_ERROR, "not loaded: %s", m->error.c_str());
				else
					log(m, SMS_LOG_INFO, "%s", m->error.c_str());
				continue;
			}
			tryLoad(m);
		}
	s_booting = false;
	// what the server mods need of the players' games: the host refuses older ones
	int clientApi = 0;
	for (SmsMod* m : g_mods)
		if (m->state == SmsMod::LOADED && m->server && m->clientApi > clientApi)
			clientApi = m->clientApi;
	if (clientApi) {
		mpg_set_min_client_api(clientApi >> 8, clientApi & 0xFF);
		port_log("[mods] players' games need mod API %d.%d (the server mods' client_api)\n", clientApi >> 8, clientApi & 0xFF);
	}
	std::string gameplay;
	for (SmsMod* m : g_mods)
		if (m->state == SmsMod::LOADED && m->gameplay)
			gameplay += " " + m->id;
	if (!gameplay.empty())
		port_log("[mods] mods that change how the game plays:%s\n", gameplay.c_str());
	const char* menuKeyName = getenv("SMS_NATIVE_MODS_MENU_KEY");
	s_menuKey               = GXPC_KeyFromName(menuKeyName && *menuKeyName ? menuKeyName : "F1");
	GXPC_SetKeyHook(keyHook);
	port_warp_want_list(); // stage names (game->area_name)
	atexit(shutdown);
}

// A mod's library changed on disk: load it again (SMS_NATIVE_MODS_HOT=1).
void hotReload()
{
	for (SmsMod* m : g_mods) {
		if (m->state != SmsMod::LOADED || m->libPath.empty())
			continue;
		long long stamp = 0;
		if (!isFile(m->libPath, &stamp) || stamp == m->libStamp)
			continue;
		log(m, SMS_LOG_INFO, "%s changed: loading it again", m->runtime.empty() ? "library" : "script");
		unloadMod(m);
		if (!loadMod(m)) {
			m->state = SmsMod::FAILED;
			log(m, SMS_LOG_ERROR, "not loaded: %s", m->error.c_str());
		}
	}
	startWaitingScripts();
}

} // namespace

// ------------------------------------------------------------------ modhost


namespace modhost {

const SmsApi g_api = {
	sizeof(SmsApi), SMS_MOD_API_MAJOR, SMS_MOD_API_MINOR, &s_core,   &s_game,  &s_player,
	&s_camera,      &g_drawApi,        &s_input,          &s_config, &s_audio, &s_native,
	&s_netTable,    &s_world,
};

void log(SmsMod* mod, int level, const char* fmt, ...)
{
	if (level <= SMS_LOG_DEBUG && !s_debug)
		return;
	char buf[2048];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof buf, fmt, ap);
	va_end(ap);
	static const char* const kLevel[] = {"debug: ", "", "warning: ", "error: "};
	port_log("[mod:%s] %s%s\n", mod ? mod->id.c_str() : "-", kLevel[level < 0 ? 0 : level > 3 ? 3 : level], buf);
}

SmsMod* current() { return s_current; }
SmsMod* setCurrent(SmsMod* mod)
{
	SmsMod* prev = s_current;
	s_current    = mod;
	return prev;
}
uint32_t frame() { return s_frame; }
int mode() { return s_mode; }
const SmsPadStatus* rawPads() { return s_raw; }

// A script mod: started by the native mod hosting its runtime.
bool loadScript(SmsMod* m)
{
	auto host = s_hosts.find(m->runtime);
	if (host == s_hosts.end()) {
		// the lua mod is optional, from the mod SDK's releases
		m->error = "needs a mod that runs " + m->runtime + " scripts" +
		           (m->runtime == "lua" ? ": the lua mod (tools/mods/get.py lua)" : "");
		return false;
	}
	m->libPath = m->dir + "/" + m->script;
	if (!isFile(m->libPath, &m->libStamp)) {
		m->error = "its script " + m->libPath + " is missing";
		return false;
	}
	m->state = SmsMod::LOADED;
	m->error.clear();
	if (m->order < 0)
		m->order = s_nextOrder++;
	SmsMod* prev = s_current;
	s_current    = m;
	int rc       = -1;
	try {
		rc = host->second.start(host->second.mod, m, host->second.user);
	} catch (...) {
		rc = -2;
	}
	s_current = prev;
	if (rc != 0) {
		if (m->error.empty())
			m->error = "its script did not start (the log says why)";
		dropRegistrations(m);
		m->state = SmsMod::FAILED;
		return false;
	}
	log(m, SMS_LOG_INFO, "loaded %s %s%s%s (%s, run by %s)", m->name.c_str(), m->version.c_str(),
	    m->author.empty() ? "" : " by ", m->author.c_str(), m->script.c_str(), host->second.mod->id.c_str());
	SmsEvMod ev = {m->id.c_str(), m->name.c_str(), m->version.c_str()};
	modh_dispatch(SMS_EV_MOD_LOADED, &ev, sizeof ev);
	return true;
}

bool loadMod(SmsMod* m)
{
	if (!m->runtime.empty())
		return loadScript(m);
	std::string looked;
	m->libPath = findLibrary(m, &looked);
	if (m->libPath.empty()) {
		m->error = std::string("no library for ") + platformDir() + " (looked for " + looked + ")";
		return false;
	}
	isFile(m->libPath, &m->libStamp);
	m->loadedPath = m->libPath;
	if (s_hot) {
		// a copy, so the build can write the library while the game has it open
		static int s_copies = 0;
		const std::string dir = dataRoot() + "/.hot";
		makeDirs(dir);
		m->loadedPath = dir + "/" + m->id + "-" + std::to_string(++s_copies) + libExt();
		if (!copyFile(m->libPath, m->loadedPath))
			m->loadedPath = m->libPath;
	}
	std::string err;
	m->handle = libOpen(m->loadedPath, &err);
	if (!m->handle) {
		m->error = "could not load " + m->libPath + ": " + err;
		return false;
	}
	SmsModVersionFn ver = (SmsModVersionFn)libSym(m->handle, "sms_mod_api_version");
	SmsModLoadFn load   = (SmsModLoadFn)libSym(m->handle, "sms_mod_load");
	m->unload           = (SmsModUnloadFn)libSym(m->handle, "sms_mod_unload");
	if (!ver || !load) {
		m->error = "not a native mod (it has no sms_mod_api_version and sms_mod_load: SMS_MOD_DEFINE())";
		libClose(m->handle);
		m->handle = nullptr;
		return false;
	}
	const uint32_t v = ver();
	if (v >> 16 != SMS_MOD_API_MAJOR) {
		char buf[128];
		snprintf(buf, sizeof buf, "made for mod API %u.%u; this game has %u.%u", (unsigned)(v >> 16),
		         (unsigned)(v & 0xFFFF), (unsigned)SMS_MOD_API_MAJOR, (unsigned)SMS_MOD_API_MINOR);
		m->error = buf;
		libClose(m->handle);
		m->handle = nullptr;
		return false;
	}
	if ((v & 0xFFFF) > SMS_MOD_API_MINOR)
		log(m, SMS_LOG_INFO, "made for mod API 1.%u (this game has 1.%u): it should check SMS_API_HAS",
		    (unsigned)(v & 0xFFFF), (unsigned)SMS_MOD_API_MINOR);
	m->state = SmsMod::LOADED; // its subscriptions in sms_mod_load are taken
	m->error.clear();
	if (m->order < 0)
		m->order = s_nextOrder++;
	SmsMod* prev = s_current;
	s_current    = m;
	int rc       = -1;
	try {
		rc = load(m, &g_api);
	} catch (...) {
		rc = -2;
	}
	s_current = prev;
	if (rc != 0) {
		m->error = rc == -2 ? "sms_mod_load threw an exception" : "sms_mod_load refused (" + std::to_string(rc) + ")";
		dropRegistrations(m);
		m->state = SmsMod::FAILED;
		libClose(m->handle);
		m->handle = nullptr;
		return false;
	}
	log(m, SMS_LOG_INFO, "loaded %s %s%s%s", m->name.c_str(), m->version.c_str(),
	    m->author.empty() ? "" : " by ", m->author.c_str());
	SmsEvMod ev = {m->id.c_str(), m->name.c_str(), m->version.c_str()};
	modh_dispatch(SMS_EV_MOD_LOADED, &ev, sizeof ev);
	if (!s_booting)
		startWaitingScripts(); // a runtime it hosts may have script mods waiting
	return true;
}

void unloadMod(SmsMod* m)
{
	if (m->state != SmsMod::LOADED)
		return;
	if (!m->runtime.empty()) {
		auto host = s_hosts.find(m->runtime);
		if (host != s_hosts.end()) {
			SmsMod* prev = s_current;
			s_current    = m;
			try {
				host->second.stop(host->second.mod, m, host->second.user);
			} catch (...) {
				log(m, SMS_LOG_ERROR, "its runtime threw an exception stopping it");
			}
			s_current = prev;
		}
		dropRegistrations(m);
		saveSettings(m);
		m->state = SmsMod::FOUND;
		return;
	}
	// the script mods it runs stop first
	for (auto& h : s_hosts)
		if (h.second.mod == m)
			for (auto it = g_mods.rbegin(); it != g_mods.rend(); ++it)
				if ((*it)->runtime == h.first && (*it)->state == SmsMod::LOADED) {
					unloadMod(*it);
					(*it)->error = "waits for " + m->id + ", which runs it";
				}
	if (m->unload) {
		SmsMod* prev = s_current;
		s_current    = m;
		try {
			m->unload(m);
		} catch (...) {
			log(m, SMS_LOG_ERROR, "sms_mod_unload threw an exception");
		}
		s_current = prev;
	}
	dropRegistrations(m);
	saveSettings(m);
	m->state = SmsMod::FOUND;
	if (m->handle)
		libClose(m->handle);
	m->handle = nullptr;
	if (s_hot && m->loadedPath != m->libPath)
		remove(m->loadedPath.c_str());
}

void setModOff(SmsMod* m, bool off) { s_pendingOff.push_back(std::make_pair(m, off)); }

std::string settingGet(SmsMod* m, const std::string& key, const std::string& def, bool* found)
{
	if (found)
		*found = false;
	if (!m)
		return def;
	readSettings(m);
	auto it = m->settings.find(key);
	if (it == m->settings.end())
		return def;
	if (found)
		*found = true;
	return it->second;
}

void settingSet(SmsMod* m, const std::string& key, const std::string& value)
{
	if (!m || key.empty())
		return;
	readSettings(m);
	std::string v = value;
	v.erase(std::remove(v.begin(), v.end(), '\n'), v.end());
	auto it = m->settings.find(key);
	if (it != m->settings.end() && it->second == v)
		return;
	m->settings[key] = v;
	m->settingsDirty = true;
}

} // namespace modhost

// ------------------------------------------------------------------ the bridge

extern "C" void modh_frame(uint32_t frame, int mode)
{
	// once multiplayer has started (its role decides which mods run, and what
	// api->net says while they load)
	if (!s_started && !mpg_started() && frame < 300) // (a build without multiplayer: not for long)
		return;
	if (!s_started)
		start();
	s_frame = frame;
	s_mode  = mode;
	if (!s_on || g_mods.empty())
		return;
	// the Mods menu's switches
	for (auto& p : s_pendingOff) {
		SmsMod* m = p.first;
		if (p.second) {
			s_offList.insert(m->id);
			if (m->state == SmsMod::LOADED)
				unloadMod(m);
			m->state = SmsMod::OFF;
			m->error = "switched off in the Mods menu";
			log(m, SMS_LOG_INFO, "switched off");
		} else {
			s_offList.erase(m->id);
			if (m->state != SmsMod::LOADED) {
				m->state = SmsMod::FOUND;
				if (!loadMod(m)) {
					m->state = SmsMod::FAILED;
					log(m, SMS_LOG_ERROR, "not loaded: %s", m->error.c_str());
				}
			}
		}
		saveOffList();
	}
	s_pendingOff.clear();
	if (s_hot && frame % 30 == 0)
		hotReload();

	netEvents();
	SmsEvFrame fe = {frame, mode};
	modh_dispatch(SMS_EV_FRAME, &fe, sizeof fe);
	drawBegin();
	SmsEvDraw de;
	drawCanvas(&de.canvas_w, &de.canvas_h);
	de.frame = frame;
	de.mode  = mode;
	modh_dispatch(SMS_EV_DRAW, &de, sizeof de);
	menuDraw();
	drawEnd();

	memset(s_keyPressed, 0, sizeof s_keyPressed);
	memset(s_pressed, 0, sizeof s_pressed);
	memset(s_released, 0, sizeof s_released);
	// settings changed this frame reach the disk within a couple of seconds
	if (now() - s_lastSave > 2.0) {
		s_lastSave = now();
		for (SmsMod* m : g_mods)
			saveSettings(m);
	}
}

extern "C" int modh_dispatch(uint32_t type, void* data, uint32_t size)
{
	if (!s_on)
		return 0;
	if (type == SMS_EV_SAVE_STATE_REQUEST)
		s_saveStateReason.clear();
	const int cancelled = dispatchTo(type, data, size, nullptr);
	if (type == SMS_EV_SAVE_STATE_REQUEST && cancelled) {
		const SmsEvSaveState* p = (const SmsEvSaveState*)data;
		s_saveStateReason       = p && p->reason ? p->reason : "A mod switched save states off";
	}
	return cancelled;
}

extern "C" int modh_wants(uint32_t type)
{
	if (type < 64)
		return s_live[type] > 0;
	auto it = s_subs.find(type);
	return it != s_subs.end() && !it->second.empty();
}

extern "C" void modh_pad_read(SmsPadStatus* pads)
{
	if (!pads)
		return;
	memcpy(s_raw, pads, sizeof s_raw);
	for (int i = 0; i < 4; i++) {
		const uint16_t held = s_raw[i].err == 0 ? s_raw[i].buttons : 0;
		s_pressed[i] |= (uint16_t)(held & ~s_prevHeld[i]);
		s_released[i] |= (uint16_t)(~held & s_prevHeld[i]);
		s_prevHeld[i] = held;
	}
	if (!s_on)
		return;
	if (menuOpen())
		menuPad(s_raw);
	if (s_live[SMS_EV_PAD_READ]) {
		SmsEvPadRead ev = {pads};
		modh_dispatch(SMS_EV_PAD_READ, &ev, sizeof ev);
	}
	// a menu has the controllers: the game sees nothing pressed
	if (menuOpen() || s_captor)
		for (int i = 0; i < 4; i++) {
			const int8_t err = pads[i].err;
			memset(&pads[i], 0, sizeof pads[i]);
			pads[i].err = err;
		}
}

extern "C" const char* modh_save_state_reason(void)
{
	return s_saveStateReason.empty() ? "A mod switched save states off" : s_saveStateReason.c_str();
}

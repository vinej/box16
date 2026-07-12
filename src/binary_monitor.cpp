// Commander X16 Emulator (Box16)
// VICE binary monitor protocol (API v2) server.
// All rights reserved. License: 2-clause BSD
//
// Implements the subset of the VICE binary monitor protocol
// (https://vice-emu.sourceforge.io/vice_13.html) that debugger front-ends
// such as VS64 use: memory and register access, exec checkpoints, stepping,
// and the STOPPED/RESUMED/CHECKPOINT_INFO event stream. Commands arrive on
// a TCP socket and are serviced from the emulator main loop between
// instructions, so no locking is needed against the debugger core.
//
// MVP limits (documented in the x16_test README): memspace 0 (main memory)
// only; checkpoint condition STRINGS (CHECKPOINT_CONDITION_SET) are
// accepted but not evaluated (the boxmon expression engine proved
// unreliable for this -- order-dependent parse state); AUTOSTART is
// acknowledged but ignored.
//
// X16 extension, second part: CHECKPOINT_SET accepts an optional binary
// word-in-ranges condition after the bank qualifier. The core evaluates
// it at CPU speed (debugger_process_cpu): the checkpoint only fires while
// the little-endian word at watch_address is inside one of the inclusive
// ranges; a miss never pauses the machine and never touches the wire.
// This is how BASIC line breakpoints run at full emulation speed: watch
// txtptr ($EE) against the tokenized program's per-line spans.
//
// X16 extension: CHECKPOINT_SET accepts an optional trailing u16 machine
// bank number after the standard memspace byte (ROM bank for $C000-$FFFF,
// RAM bank for $A000-$BFFF), so exec checkpoints inside banked ROM/RAM
// fire only while that bank is selected -- required to hook the BASIC
// interpreter (e.g. newstt at $CC21, BASIC ROM bank 4) without stray hits
// from the KERNAL/DOS banks. Standard 8/9-byte VICE bodies keep the old
// behavior (bank 0).

#include "binary_monitor.h"

#ifdef _WIN32
// compat.h is force-included into every translation unit and pulls in
// <windows.h>, which brings the winsock 1.1 API with it -- winsock2.h can
// no longer be included after that. Everything this server needs (socket,
// bind, listen, accept, recv, send, ioctlsocket, WSAStartup) already
// exists in the 1.1 API, and ws2_32.lib exports all of it.
#	include <winsock.h>
#	pragma comment(lib, "ws2_32.lib")
#	pragma comment(lib, "winmm.lib")
using socket_t = SOCKET;
static constexpr socket_t INVALID_SOCK = INVALID_SOCKET;
#else
#	include <arpa/inet.h>
#	include <errno.h>
#	include <fcntl.h>
#	include <netinet/in.h>
#	include <netinet/tcp.h>
#	include <sys/socket.h>
#	include <unistd.h>
using socket_t = int;
static constexpr socket_t INVALID_SOCK = -1;
#endif

#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <vector>

#include <fmt/format.h>

#include "SDL.h"

#include "cpu/fake6502.h"
#include "debugger.h"
#include "glue.h"
#include "hypercalls.h"
#include "memory.h"

// Set the environment variable BOX16_BINMON_LOG to a file path to record a
// timestamped wire trace (every rx/tx frame plus run-state notes) for
// diagnosing client interactions such as VS64's attach handshake.

namespace {

FILE *Log_file = nullptr;

const char *command_name(uint8_t cmd);

void log_open()
{
	const char *path = getenv("BOX16_BINMON_LOG");
	if (path && *path) {
		Log_file = fopen(path, "w");
		if (Log_file) {
			fmt::print(Log_file, "# box16 binary monitor wire trace\n");
			fflush(Log_file);
		}
	}
}

void log_line(const std::string &s)
{
	if (Log_file) {
		fmt::print(Log_file, "{}\n", s);
		fflush(Log_file);
	}
}

void log_frame(const char *dir, const uint8_t *data, size_t len, const char *note)
{
	if (!Log_file) {
		return;
	}
	std::string hex;
	for (size_t i = 0; i < len && i < 48; ++i) {
		hex += fmt::format("{:02x} ", data[i]);
	}
	fmt::print(Log_file, "{} [{:3} B] {:22} {}{}\n", dir, len, note, hex, len > 48 ? "..." : "");
	fflush(Log_file);
}

constexpr uint8_t PROTOCOL_STX  = 0x02;
constexpr uint8_t PROTOCOL_API  = 0x02;
constexpr uint32_t EVENT_REQUEST_ID = 0xffffffff;

// Command types (requests)
constexpr uint8_t CMD_MEMORY_GET               = 0x01;
constexpr uint8_t CMD_MEMORY_SET               = 0x02;
constexpr uint8_t CMD_CHECKPOINT_GET           = 0x11;
constexpr uint8_t CMD_CHECKPOINT_SET           = 0x12;
constexpr uint8_t CMD_CHECKPOINT_DELETE        = 0x13;
constexpr uint8_t CMD_CHECKPOINT_LIST          = 0x14;
constexpr uint8_t CMD_CHECKPOINT_TOGGLE        = 0x15;
constexpr uint8_t CMD_CHECKPOINT_CONDITION_SET = 0x22;
constexpr uint8_t CMD_REGISTERS_GET            = 0x31;
constexpr uint8_t CMD_REGISTERS_SET            = 0x32;
constexpr uint8_t CMD_ADVANCE_INSTRUCTIONS     = 0x71;
constexpr uint8_t CMD_EXECUTE_UNTIL_RETURN     = 0x73;
constexpr uint8_t CMD_PING                     = 0x81;
constexpr uint8_t CMD_BANKS_AVAILABLE          = 0x82;
constexpr uint8_t CMD_REGISTERS_AVAILABLE      = 0x83;
constexpr uint8_t CMD_VICE_INFO                = 0x85;
constexpr uint8_t CMD_EXIT                     = 0xaa;
constexpr uint8_t CMD_QUIT                     = 0xbb;
constexpr uint8_t CMD_RESET                    = 0xcc;
constexpr uint8_t CMD_AUTOSTART                = 0xdd;

// Response/event types that differ from their request type
constexpr uint8_t RESPONSE_CHECKPOINT_INFO = 0x11;
constexpr uint8_t RESPONSE_JAM             = 0x61;
constexpr uint8_t RESPONSE_STOPPED         = 0x62;
constexpr uint8_t RESPONSE_RESUMED         = 0x63;

// Error codes
constexpr uint8_t ERR_OK              = 0x00;
constexpr uint8_t ERR_OBJECT_MISSING  = 0x01;
constexpr uint8_t ERR_INVALID_MEMSPACE = 0x02;
constexpr uint8_t ERR_INVALID_LENGTH  = 0x80;
constexpr uint8_t ERR_INVALID_PARAM   = 0x81;
constexpr uint8_t ERR_INVALID_COMMAND = 0x83;

// Checkpoint CPU operation bits
constexpr uint8_t CHECKPOINT_OP_LOAD  = 0x01;
constexpr uint8_t CHECKPOINT_OP_STORE = 0x02;
constexpr uint8_t CHECKPOINT_OP_EXEC  = 0x04;

// Register ids reported via REGISTERS_AVAILABLE. VS64 matches these by
// their lowercase *names*, so the names matter and the ids are ours to pick.
constexpr uint8_t REG_ID_A  = 0;
constexpr uint8_t REG_ID_X  = 1;
constexpr uint8_t REG_ID_Y  = 2;
constexpr uint8_t REG_ID_PC = 3;
constexpr uint8_t REG_ID_SP = 4;
constexpr uint8_t REG_ID_FL = 5;

struct register_def {
	uint8_t     id;
	uint8_t     bits;
	const char *name;
};

constexpr register_def Register_defs[] = {
	{ REG_ID_A, 8, "a" },
	{ REG_ID_X, 8, "x" },
	{ REG_ID_Y, 8, "y" },
	{ REG_ID_PC, 16, "pc" },
	{ REG_ID_SP, 8, "sp" },
	{ REG_ID_FL, 8, "fl" },
};

struct bank_def {
	uint16_t    id;
	const char *name;
};

constexpr bank_def Bank_defs[] = {
	{ 0, "cpu" },
	{ 1, "ram" },
	{ 2, "rom" },
	{ 3, "io" },
};

struct checkpoint_t {
	uint32_t    num;
	uint16_t    start;
	uint16_t    end;
	bool        stop;
	bool        enabled;
	uint8_t     op; // CHECKPOINT_OP_* bits
	bool        temporary;
	uint8_t     bank; // machine bank (X16 extension); 0 from standard VICE clients
	// X16 extension: optional word-in-ranges condition (empty = unconditional)
	uint16_t                                   cond_watch = 0;
	std::vector<std::pair<uint16_t, uint16_t>> cond_ranges;
	uint32_t    hit_count;
	uint32_t    ignore_count;
	std::string condition;
};

socket_t Listen_socket = INVALID_SOCK;
socket_t Client_socket = INVALID_SOCK;
bool     Initialized   = false;

std::vector<uint8_t> Recv_buffer;

std::map<uint32_t, checkpoint_t> Checkpoints;
uint32_t Next_checkpoint_num = 1;

bool Was_paused = false;

// ------------------------------------------------------------------
// Source line table (from Oscar64's .dbj), used for line-granular
// stepping. Line_key_map[addr] identifies the source line at that
// address; -1 means "no source line here" (ROM, library asm, etc.).
// Two addresses share a key iff they are the same source file + line.
// ------------------------------------------------------------------
constexpr int32_t NO_LINE = -1;
std::vector<int32_t> Line_key_map; // indexed by 16-bit address
bool                 Line_map_loaded = false;

// Line stepping runs the CPU one instruction at a time inside the emulator
// (no client round-trips) until the source line changes, so one client
// ADVANCE advances one C line -- and a whole __asm{} block, being a single
// source line, is stepped over as one unit whether F10 or F11 is used.
bool    Line_stepping         = false;
bool    Line_step_over        = false;
int32_t Line_step_start_key   = NO_LINE;
uint32_t Line_step_guard      = 0;
// A single C line should never be more than a few thousand instructions;
// this only bounds pathological cases (e.g. a spin loop) so a step cannot
// wedge the emulator.
constexpr uint32_t LINE_STEP_MAX = 2000000;

int32_t line_key(uint16_t addr)
{
	return Line_map_loaded ? Line_key_map[addr] : NO_LINE;
}

// Defined in the Events section below; needed by cmd_advance above it.
void send_pc_event(uint8_t event_type);
void send_registers_event();

// ------------------------------------------------------------------
// Source line table loading
// ------------------------------------------------------------------

// Pull the integer that follows a `"key":` token, e.g. "start": 2176.
bool json_int(const std::string &line, const char *key, long &out)
{
	const size_t k = line.find(key);
	if (k == std::string::npos) {
		return false;
	}
	size_t p = k + strlen(key);
	while (p < line.size() && (line[p] == ' ' || line[p] == ':')) {
		++p;
	}
	if (p >= line.size() || (!isdigit((unsigned char)line[p]) && line[p] != '-')) {
		return false;
	}
	out = strtol(line.c_str() + p, nullptr, 10);
	return true;
}

std::string json_str(const std::string &line, const char *key)
{
	const size_t k = line.find(key);
	if (k == std::string::npos) {
		return "";
	}
	size_t p = line.find('"', k + strlen(key));
	if (p == std::string::npos) {
		return "";
	}
	++p;
	const size_t e = line.find('"', p);
	return e == std::string::npos ? "" : line.substr(p, e - p);
}

// Parse the .dbj sitting next to the -prg. Its per-instruction line entries
// are one JSON object per text line beginning with `{"start":` and carrying
// only start/end/source/line -- the function/symbol headers begin with
// `{"name":`, so a prefix test tells them apart without a JSON parser.
void load_line_map(const std::string &dbj_path)
{
	std::ifstream f(dbj_path);
	if (!f) {
		return;
	}
	Line_key_map.assign(0x10000, NO_LINE);
	std::map<std::string, int32_t> source_ids;
	int32_t                        next_source_id = 0;
	size_t                         entries        = 0;

	std::string raw;
	while (std::getline(f, raw)) {
		size_t s = raw.find_first_not_of(" \t");
		if (s == std::string::npos || raw.compare(s, 9, "{\"start\":") != 0) {
			continue;
		}
		long start, end, ln;
		if (!json_int(raw, "\"start\"", start) || !json_int(raw, "\"end\"", end) || !json_int(raw, "\"line\"", ln)) {
			continue;
		}
		if (start < 0 || end > 0x10000 || end < start) {
			continue;
		}
		const std::string src = json_str(raw, "\"source\"");
		auto              it  = source_ids.find(src);
		int32_t           sid;
		if (it == source_ids.end()) {
			sid              = next_source_id++;
			source_ids[src]  = sid;
		} else {
			sid = it->second;
		}
		const int32_t key = sid * 1000000 + static_cast<int32_t>(ln);
		for (long a = start; a < end; ++a) {
			Line_key_map[a] = key;
		}
		++entries;
	}

	if (entries > 0) {
		Line_map_loaded = true;
		fmt::print("binary monitor: loaded {} source line spans from {}\n", entries, dbj_path);
		fflush(stdout);
	}
}

// ------------------------------------------------------------------
// Socket plumbing
// ------------------------------------------------------------------

void set_nonblocking(socket_t s)
{
#ifdef _WIN32
	u_long mode = 1;
	ioctlsocket(s, FIONBIO, &mode);
#else
	fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) | O_NONBLOCK);
#endif
}

void close_socket(socket_t &s)
{
	if (s != INVALID_SOCK) {
#ifdef _WIN32
		closesocket(s);
#else
		close(s);
#endif
		s = INVALID_SOCK;
	}
}

bool would_block()
{
#ifdef _WIN32
	return WSAGetLastError() == WSAEWOULDBLOCK;
#else
	return errno == EWOULDBLOCK || errno == EAGAIN;
#endif
}

// Debugger front-ends and the emulator exchange only tiny frames, so a
// short blocking retry loop on the nonblocking socket is enough.
void send_all(const uint8_t *data, size_t len)
{
	if (Client_socket == INVALID_SOCK) {
		return;
	}
	if (Log_file && len >= 7) {
		const char *note = data[6] == RESPONSE_STOPPED ? "STOPPED"
		                 : data[6] == RESPONSE_RESUMED ? "RESUMED"
		                 : data[6] == RESPONSE_CHECKPOINT_INFO ? "CHECKPOINT_INFO"
		                 : "response";
		log_frame("tx", data, len, note);
	}
	size_t sent    = 0;
	int    retries = 0;
	while (sent < len) {
		const int n = send(Client_socket, reinterpret_cast<const char *>(data) + sent, static_cast<int>(len - sent), 0);
		if (n > 0) {
			sent += n;
			retries = 0;
		} else if (would_block() && retries < 1000) {
			++retries;
			SDL_Delay(1);
		} else {
			close_socket(Client_socket);
			return;
		}
	}
}

// ------------------------------------------------------------------
// Frame encoding
// ------------------------------------------------------------------

struct body_writer {
	std::vector<uint8_t> bytes;

	void u8(uint8_t v)
	{
		bytes.push_back(v);
	}
	void u16(uint16_t v)
	{
		bytes.push_back(v & 0xff);
		bytes.push_back((v >> 8) & 0xff);
	}
	void u32(uint32_t v)
	{
		bytes.push_back(v & 0xff);
		bytes.push_back((v >> 8) & 0xff);
		bytes.push_back((v >> 16) & 0xff);
		bytes.push_back((v >> 24) & 0xff);
	}
	void str(const char *s)
	{
		while (*s) {
			bytes.push_back(static_cast<uint8_t>(*s++));
		}
	}
};

void send_response(uint8_t response_type, uint8_t error_code, uint32_t request_id, const body_writer &body)
{
	std::vector<uint8_t> frame;
	const uint32_t       body_len = static_cast<uint32_t>(body.bytes.size());
	frame.reserve(12 + body.bytes.size());
	frame.push_back(PROTOCOL_STX);
	frame.push_back(PROTOCOL_API);
	frame.push_back(body_len & 0xff);
	frame.push_back((body_len >> 8) & 0xff);
	frame.push_back((body_len >> 16) & 0xff);
	frame.push_back((body_len >> 24) & 0xff);
	frame.push_back(response_type);
	frame.push_back(error_code);
	frame.push_back(request_id & 0xff);
	frame.push_back((request_id >> 8) & 0xff);
	frame.push_back((request_id >> 16) & 0xff);
	frame.push_back((request_id >> 24) & 0xff);
	frame.insert(frame.end(), body.bytes.begin(), body.bytes.end());
	send_all(frame.data(), frame.size());
}

void send_empty_response(uint8_t response_type, uint8_t error_code, uint32_t request_id)
{
	send_response(response_type, error_code, request_id, body_writer{});
}

// ------------------------------------------------------------------
// Request decoding helpers
// ------------------------------------------------------------------

struct body_reader {
	const uint8_t *data;
	size_t         len;
	size_t         pos = 0;

	bool have(size_t n) const
	{
		return pos + n <= len;
	}
	uint8_t u8()
	{
		return data[pos++];
	}
	uint16_t u16()
	{
		const uint16_t v = static_cast<uint16_t>(data[pos]) | (static_cast<uint16_t>(data[pos + 1]) << 8);
		pos += 2;
		return v;
	}
	uint32_t u32()
	{
		const uint32_t v = static_cast<uint32_t>(data[pos]) | (static_cast<uint32_t>(data[pos + 1]) << 8) |
		                   (static_cast<uint32_t>(data[pos + 2]) << 16) | (static_cast<uint32_t>(data[pos + 3]) << 24);
		pos += 4;
		return v;
	}
};

// ------------------------------------------------------------------
// State snapshots
// ------------------------------------------------------------------

// The PC the debugger considers current: when an exec breakpoint fires the
// CPU core rolls the fetch back, so state6502.pc already points AT the
// breakpointed (not yet executed) instruction -- same semantics as VICE.
uint16_t current_pc()
{
	return state6502.pc - waiting;
}

uint16_t register_value(uint8_t id)
{
	switch (id) {
		case REG_ID_A: return state6502.a;
		case REG_ID_X: return state6502.x;
		case REG_ID_Y: return state6502.y;
		case REG_ID_PC: return current_pc();
		case REG_ID_SP: return state6502.sp;
		case REG_ID_FL: return state6502.status;
		default: return 0;
	}
}

void write_registers_body(body_writer &body)
{
	body.u16(static_cast<uint16_t>(std::size(Register_defs)));
	for (const auto &reg : Register_defs) {
		body.u8(3); // item size, excluding this byte
		body.u8(reg.id);
		body.u16(register_value(reg.id));
	}
}

void write_checkpoint_info_body(body_writer &body, const checkpoint_t &cp, bool currently_hit)
{
	body.u32(cp.num);
	body.u8(currently_hit ? 1 : 0);
	body.u16(cp.start);
	body.u16(cp.end);
	body.u8(cp.stop ? 1 : 0);
	body.u8(cp.enabled ? 1 : 0);
	body.u8(cp.op);
	body.u8(cp.temporary ? 1 : 0);
	body.u32(cp.hit_count);
	body.u32(cp.ignore_count);
	body.u8(cp.condition.empty() ? 0 : 1);
	body.u8(0); // memspace: main memory
}

// ------------------------------------------------------------------
// Checkpoint <-> Box16 breakpoint mapping
// ------------------------------------------------------------------

uint8_t checkpoint_op_to_debug_flags(uint8_t op)
{
	uint8_t flags = 0;
	if (op & CHECKPOINT_OP_EXEC) {
		flags |= DEBUG6502_EXEC;
	}
	if (op & CHECKPOINT_OP_LOAD) {
		flags |= DEBUG6502_READ;
	}
	if (op & CHECKPOINT_OP_STORE) {
		flags |= DEBUG6502_WRITE;
	}
	return flags;
}

void apply_checkpoint_range(const checkpoint_t &cp, void (*fn)(uint16_t, uint8_t, uint8_t))
{
	const uint8_t flags = checkpoint_op_to_debug_flags(cp.op);
	if (flags == 0) {
		return;
	}
	for (uint32_t addr = cp.start; addr <= cp.end; ++addr) {
		fn(static_cast<uint16_t>(addr), cp.bank, flags);
	}
}

// The debugger core normalizes sub-$A000 breakpoints to bank 0; condition
// helpers must address the same slot.
uint8_t effective_bank(uint16_t addr, uint8_t bank)
{
	return addr < 0xa000 ? 0 : bank;
}

void clear_checkpoint_condition(checkpoint_t &cp)
{
	if (cp.cond_ranges.empty()) {
		return;
	}
	for (uint32_t addr = cp.start; addr <= cp.end; ++addr) {
		debugger_clear_word_range_condition(static_cast<uint16_t>(addr), effective_bank(static_cast<uint16_t>(addr), cp.bank));
	}
	cp.cond_ranges.clear();
}

void remove_all_checkpoints()
{
	for (auto &[num, cp] : Checkpoints) {
		clear_checkpoint_condition(cp);
		apply_checkpoint_range(cp, debugger_remove_breakpoint);
	}
	Checkpoints.clear();
}

// The checkpoint whose exec range covers the paused PC, if any.
checkpoint_t *find_hit_checkpoint(uint16_t pc)
{
	const uint8_t pc_bank = memory_get_current_bank(pc);
	for (auto &[num, cp] : Checkpoints) {
		if (cp.enabled && (cp.op & CHECKPOINT_OP_EXEC) && pc >= cp.start && pc <= cp.end) {
			// below $A000 the debugger core normalizes every breakpoint to
			// bank 0 (debugger_add_breakpoint); mirror that here
			const uint8_t cp_bank = (pc < 0xa000) ? 0 : cp.bank;
			if (cp_bank == pc_bank) {
				return &cp;
			}
		}
	}
	return nullptr;
}

// ------------------------------------------------------------------
// Command handlers
// ------------------------------------------------------------------

void cmd_memory_get(body_reader &req, uint32_t request_id)
{
	if (!req.have(8)) {
		send_empty_response(CMD_MEMORY_GET, ERR_INVALID_LENGTH, request_id);
		return;
	}
	req.u8(); // side effects: reads are always side-effect-free here
	const uint16_t start    = req.u16();
	const uint16_t end      = req.u16();
	const uint8_t  memspace = req.u8();
	req.u16(); // bank id: MVP serves the current CPU view only

	if (memspace != 0) {
		send_empty_response(CMD_MEMORY_GET, ERR_INVALID_MEMSPACE, request_id);
		return;
	}
	if (end < start) {
		send_empty_response(CMD_MEMORY_GET, ERR_INVALID_PARAM, request_id);
		return;
	}

	body_writer body;
	const uint32_t count = static_cast<uint32_t>(end) - start + 1;
	body.u16(static_cast<uint16_t>(count));
	for (uint32_t addr = start; addr <= end; ++addr) {
		body.u8(debug_read6502(static_cast<uint16_t>(addr)));
	}
	send_response(CMD_MEMORY_GET, ERR_OK, request_id, body);
}

void cmd_memory_set(body_reader &req, uint32_t request_id)
{
	if (!req.have(8)) {
		send_empty_response(CMD_MEMORY_SET, ERR_INVALID_LENGTH, request_id);
		return;
	}
	req.u8(); // side effects
	const uint16_t start    = req.u16();
	const uint16_t end      = req.u16();
	const uint8_t  memspace = req.u8();
	req.u16(); // bank id

	if (memspace != 0) {
		send_empty_response(CMD_MEMORY_SET, ERR_INVALID_MEMSPACE, request_id);
		return;
	}
	const uint32_t count = static_cast<uint32_t>(end) - start + 1;
	if (end < start || !req.have(count)) {
		send_empty_response(CMD_MEMORY_SET, ERR_INVALID_LENGTH, request_id);
		return;
	}
	for (uint32_t addr = start; addr <= end; ++addr) {
		debug_write6502(static_cast<uint16_t>(addr), memory_get_current_bank(static_cast<uint16_t>(addr)), req.u8());
	}
	send_empty_response(CMD_MEMORY_SET, ERR_OK, request_id);
}

void cmd_checkpoint_get(body_reader &req, uint32_t request_id)
{
	if (!req.have(4)) {
		send_empty_response(RESPONSE_CHECKPOINT_INFO, ERR_INVALID_LENGTH, request_id);
		return;
	}
	const uint32_t num = req.u32();
	const auto     it  = Checkpoints.find(num);
	if (it == Checkpoints.end()) {
		send_empty_response(RESPONSE_CHECKPOINT_INFO, ERR_OBJECT_MISSING, request_id);
		return;
	}
	body_writer body;
	write_checkpoint_info_body(body, it->second, false);
	send_response(RESPONSE_CHECKPOINT_INFO, ERR_OK, request_id, body);
}

void cmd_checkpoint_set(body_reader &req, uint32_t request_id)
{
	// VICE allows the trailing memspace byte to be omitted; VS64 sends it.
	if (!req.have(8)) {
		send_empty_response(RESPONSE_CHECKPOINT_INFO, ERR_INVALID_LENGTH, request_id);
		return;
	}
	checkpoint_t cp;
	cp.num          = Next_checkpoint_num++;
	cp.start        = req.u16();
	cp.end          = req.u16();
	cp.stop         = req.u8() != 0;
	cp.enabled      = req.u8() != 0;
	cp.op           = req.u8();
	cp.temporary    = req.u8() != 0;
	cp.hit_count    = 0;
	cp.ignore_count = 0;
	cp.bank         = 0;
	if (req.have(1) && req.u8() != 0) {
		send_empty_response(RESPONSE_CHECKPOINT_INFO, ERR_INVALID_MEMSPACE, request_id);
		return;
	}
	if (req.have(2)) { // X16 extension: optional machine bank qualifier
		const uint16_t bank = req.u16();
		if (bank > 0xff) {
			send_empty_response(RESPONSE_CHECKPOINT_INFO, ERR_INVALID_PARAM, request_id);
			return;
		}
		cp.bank = static_cast<uint8_t>(bank);
	}
	if (req.have(1)) { // X16 extension: optional word-in-ranges condition
		const uint8_t cond_type = req.u8();
		if (cond_type != 1 || !req.have(3)) {
			send_empty_response(RESPONSE_CHECKPOINT_INFO, ERR_INVALID_PARAM, request_id);
			return;
		}
		cp.cond_watch       = req.u16();
		const uint8_t count = req.u8();
		if (!req.have(static_cast<uint32_t>(count) * 4)) {
			send_empty_response(RESPONSE_CHECKPOINT_INFO, ERR_INVALID_LENGTH, request_id);
			return;
		}
		for (uint8_t i = 0; i < count; ++i) {
			const uint16_t lo = req.u16();
			const uint16_t hi = req.u16();
			cp.cond_ranges.emplace_back(lo, hi);
		}
	}
	if (cp.end < cp.start) {
		send_empty_response(RESPONSE_CHECKPOINT_INFO, ERR_INVALID_PARAM, request_id);
		return;
	}

	apply_checkpoint_range(cp, debugger_add_breakpoint);
	if (!cp.enabled) {
		apply_checkpoint_range(cp, debugger_deactivate_breakpoint);
	}
	for (uint32_t addr = cp.start; !cp.cond_ranges.empty() && addr <= cp.end; ++addr) {
		debugger_set_word_range_condition(static_cast<uint16_t>(addr), effective_bank(static_cast<uint16_t>(addr), cp.bank),
		                                  cp.cond_watch, cp.cond_ranges);
	}
	Checkpoints[cp.num] = cp;

	body_writer body;
	write_checkpoint_info_body(body, cp, false);
	send_response(RESPONSE_CHECKPOINT_INFO, ERR_OK, request_id, body);
}

void cmd_checkpoint_delete(body_reader &req, uint32_t request_id)
{
	if (!req.have(4)) {
		send_empty_response(CMD_CHECKPOINT_DELETE, ERR_INVALID_LENGTH, request_id);
		return;
	}
	const uint32_t num = req.u32();
	const auto     it  = Checkpoints.find(num);
	if (it == Checkpoints.end()) {
		send_empty_response(CMD_CHECKPOINT_DELETE, ERR_OBJECT_MISSING, request_id);
		return;
	}
	clear_checkpoint_condition(it->second);
	apply_checkpoint_range(it->second, debugger_remove_breakpoint);
	Checkpoints.erase(it);
	send_empty_response(CMD_CHECKPOINT_DELETE, ERR_OK, request_id);
}

void cmd_checkpoint_list(uint32_t request_id)
{
	for (const auto &[num, cp] : Checkpoints) {
		body_writer info;
		write_checkpoint_info_body(info, cp, false);
		send_response(RESPONSE_CHECKPOINT_INFO, ERR_OK, request_id, info);
	}
	body_writer body;
	body.u32(static_cast<uint32_t>(Checkpoints.size()));
	send_response(CMD_CHECKPOINT_LIST, ERR_OK, request_id, body);
}

void cmd_checkpoint_toggle(body_reader &req, uint32_t request_id)
{
	if (!req.have(5)) {
		send_empty_response(CMD_CHECKPOINT_TOGGLE, ERR_INVALID_LENGTH, request_id);
		return;
	}
	const uint32_t num     = req.u32();
	const bool     enabled = req.u8() != 0;
	const auto     it      = Checkpoints.find(num);
	if (it == Checkpoints.end()) {
		send_empty_response(CMD_CHECKPOINT_TOGGLE, ERR_OBJECT_MISSING, request_id);
		return;
	}
	it->second.enabled = enabled;
	apply_checkpoint_range(it->second, enabled ? debugger_activate_breakpoint : debugger_deactivate_breakpoint);
	send_empty_response(CMD_CHECKPOINT_TOGGLE, ERR_OK, request_id);
}

void cmd_checkpoint_condition_set(body_reader &req, uint32_t request_id)
{
	if (!req.have(5)) {
		send_empty_response(CMD_CHECKPOINT_CONDITION_SET, ERR_INVALID_LENGTH, request_id);
		return;
	}
	const uint32_t num     = req.u32();
	const uint8_t  cond_len = req.u8();
	if (!req.have(cond_len)) {
		send_empty_response(CMD_CHECKPOINT_CONDITION_SET, ERR_INVALID_LENGTH, request_id);
		return;
	}
	const auto it = Checkpoints.find(num);
	if (it == Checkpoints.end()) {
		send_empty_response(CMD_CHECKPOINT_CONDITION_SET, ERR_OBJECT_MISSING, request_id);
		return;
	}
	// Stored but not evaluated: the boxmon expression engine is not reliable
	// enough here (order-dependent parse state). Fast conditional checkpoints
	// use the binary word-in-ranges extension of CHECKPOINT_SET instead.
	it->second.condition.assign(reinterpret_cast<const char *>(req.data + req.pos), cond_len);
	send_empty_response(CMD_CHECKPOINT_CONDITION_SET, ERR_OK, request_id);
}

void cmd_registers_get(body_reader &req, uint32_t request_id)
{
	if (req.have(1) && req.u8() != 0) {
		send_empty_response(CMD_REGISTERS_GET, ERR_INVALID_MEMSPACE, request_id);
		return;
	}
	body_writer body;
	write_registers_body(body);
	send_response(CMD_REGISTERS_GET, ERR_OK, request_id, body);
}

void cmd_registers_set(body_reader &req, uint32_t request_id)
{
	if (!req.have(3)) {
		send_empty_response(CMD_REGISTERS_GET, ERR_INVALID_LENGTH, request_id);
		return;
	}
	if (req.u8() != 0) {
		send_empty_response(CMD_REGISTERS_GET, ERR_INVALID_MEMSPACE, request_id);
		return;
	}
	const uint16_t count = req.u16();
	for (uint16_t i = 0; i < count; ++i) {
		if (!req.have(2)) {
			send_empty_response(CMD_REGISTERS_GET, ERR_INVALID_LENGTH, request_id);
			return;
		}
		const uint8_t item_size = req.u8();
		if (!req.have(item_size)) {
			send_empty_response(CMD_REGISTERS_GET, ERR_INVALID_LENGTH, request_id);
			return;
		}
		const size_t  item_end = req.pos + item_size;
		const uint8_t id       = req.u8();
		const uint16_t value   = req.u16();
		switch (id) {
			case REG_ID_A: state6502.a = value & 0xff; break;
			case REG_ID_X: state6502.x = value & 0xff; break;
			case REG_ID_Y: state6502.y = value & 0xff; break;
			case REG_ID_PC: state6502.pc = value; break;
			case REG_ID_SP: state6502.sp = value & 0xff; break;
			case REG_ID_FL: state6502.status = value & 0xff; break;
			default: break;
		}
		req.pos = item_end;
	}
	body_writer body;
	write_registers_body(body);
	send_response(CMD_REGISTERS_GET, ERR_OK, request_id, body);
}

void cmd_advance_instructions(body_reader &req, uint32_t request_id)
{
	if (!req.have(3)) {
		send_empty_response(CMD_ADVANCE_INSTRUCTIONS, ERR_INVALID_LENGTH, request_id);
		return;
	}
	const bool     step_over = req.u8() != 0;
	const uint16_t count     = req.u16();
	send_empty_response(CMD_ADVANCE_INSTRUCTIONS, ERR_OK, request_id);

	// With a source line table, advance a whole C line internally rather
	// than a single instruction: one client step = one source line, and an
	// __asm{} block (a single source line spanning many instructions) is
	// stepped over as one unit for both F10 and F11. The per-instruction
	// grind happens inside the emulator instead of as client round-trips.
	if (Line_map_loaded && line_key(current_pc()) != NO_LINE) {
		Line_stepping       = true;
		Line_step_over      = step_over;
		Line_step_start_key = line_key(current_pc());
		Line_step_guard     = 0;
		Was_paused          = false; // suppress the normal transition events
		send_pc_event(RESPONSE_RESUMED);
	}

	if (step_over) {
		debugger_step_over_execution();
	} else {
		debugger_step_execution(count > 1 ? count : 0);
	}
}

void cmd_execute_until_return(uint32_t request_id)
{
	send_empty_response(CMD_EXECUTE_UNTIL_RETURN, ERR_OK, request_id);
	debugger_step_out_execution();
}

void cmd_banks_available(uint32_t request_id)
{
	body_writer body;
	body.u16(static_cast<uint16_t>(std::size(Bank_defs)));
	for (const auto &bank : Bank_defs) {
		const uint8_t name_len = static_cast<uint8_t>(strlen(bank.name));
		body.u8(3 + name_len); // item size, excluding this byte
		body.u16(bank.id);
		body.u8(name_len);
		body.str(bank.name);
	}
	send_response(CMD_BANKS_AVAILABLE, ERR_OK, request_id, body);
}

void cmd_registers_available(body_reader &req, uint32_t request_id)
{
	if (req.have(1) && req.u8() != 0) {
		send_empty_response(CMD_REGISTERS_AVAILABLE, ERR_INVALID_MEMSPACE, request_id);
		return;
	}
	body_writer body;
	body.u16(static_cast<uint16_t>(std::size(Register_defs)));
	for (const auto &reg : Register_defs) {
		const uint8_t name_len = static_cast<uint8_t>(strlen(reg.name));
		body.u8(3 + name_len); // item size, excluding this byte
		body.u8(reg.id);
		body.u8(reg.bits);
		body.u8(name_len);
		body.str(reg.name);
	}
	send_response(CMD_REGISTERS_AVAILABLE, ERR_OK, request_id, body);
}

void cmd_vice_info(uint32_t request_id)
{
	body_writer body;
	// Present as VICE 3.5 so clients enable their API-v2 feature set.
	body.u8(4);
	body.u8(3);
	body.u8(5);
	body.u8(0);
	body.u8(0);
	body.u8(4);
	body.u32(0); // svn revision
	send_response(CMD_VICE_INFO, ERR_OK, request_id, body);
}

void cmd_exit(uint32_t request_id)
{
	send_empty_response(CMD_EXIT, ERR_OK, request_id);
	debugger_continue_execution();
}

void cmd_quit(uint32_t request_id)
{
	send_empty_response(CMD_QUIT, ERR_OK, request_id);
	SDL_Event evt;
	SDL_zero(evt);
	evt.type = SDL_QUIT;
	SDL_PushEvent(&evt);
}

// Reset, then re-arm Box16's boot loader from the -prg the emulator was
// launched with, so the program is injected and RUN again once the KERNAL
// comes back up. VS64 sends RESET immediately followed by AUTOSTART on
// every connect (attach included); reloading here means the program runs
// to break into, instead of the machine sitting at the BASIC prompt.
//
// Crucially, the machine is left PAUSED at the reset vector, not running:
// VS64 installs its breakpoints AFTER the reset (via configurationDone)
// and only then resumes with CMD_EXIT. If we resumed here, the program
// would boot and run past main() before a single breakpoint was armed --
// so a breakpoint on the first line of main() would never be hit. Holding
// the reset lets those breakpoints land first. The STOPPED event for this
// internal pause is suppressed (Was_paused pre-synced) so the client sees
// a clean run that stops only at its own breakpoints.
void cmd_reset(uint32_t request_id)
{
	send_empty_response(CMD_RESET, ERR_OK, request_id);
	machine_reset();
	hypercalls_init();
	debugger_pause_execution();
	Was_paused = true;
}

const char *command_name(uint8_t cmd)
{
	switch (cmd) {
		case CMD_MEMORY_GET: return "MEMORY_GET";
		case CMD_MEMORY_SET: return "MEMORY_SET";
		case CMD_CHECKPOINT_GET: return "CHECKPOINT_GET";
		case CMD_CHECKPOINT_SET: return "CHECKPOINT_SET";
		case CMD_CHECKPOINT_DELETE: return "CHECKPOINT_DELETE";
		case CMD_CHECKPOINT_LIST: return "CHECKPOINT_LIST";
		case CMD_CHECKPOINT_TOGGLE: return "CHECKPOINT_TOGGLE";
		case CMD_CHECKPOINT_CONDITION_SET: return "CONDITION_SET";
		case CMD_REGISTERS_GET: return "REGISTERS_GET";
		case CMD_REGISTERS_SET: return "REGISTERS_SET";
		case CMD_ADVANCE_INSTRUCTIONS: return "ADVANCE_INSTR";
		case CMD_EXECUTE_UNTIL_RETURN: return "UNTIL_RETURN";
		case CMD_PING: return "PING";
		case CMD_BANKS_AVAILABLE: return "BANKS_AVAIL";
		case CMD_REGISTERS_AVAILABLE: return "REGISTERS_AVAIL";
		case CMD_VICE_INFO: return "VICE_INFO";
		case CMD_EXIT: return "EXIT";
		case CMD_QUIT: return "QUIT";
		case CMD_RESET: return "RESET";
		case CMD_AUTOSTART: return "AUTOSTART";
		default: return "UNKNOWN";
	}
}

void dispatch(uint8_t command, uint32_t request_id, const uint8_t *body, size_t body_len)
{
	body_reader req{ body, body_len };
	switch (command) {
		case CMD_MEMORY_GET: cmd_memory_get(req, request_id); break;
		case CMD_MEMORY_SET: cmd_memory_set(req, request_id); break;
		case CMD_CHECKPOINT_GET: cmd_checkpoint_get(req, request_id); break;
		case CMD_CHECKPOINT_SET: cmd_checkpoint_set(req, request_id); break;
		case CMD_CHECKPOINT_DELETE: cmd_checkpoint_delete(req, request_id); break;
		case CMD_CHECKPOINT_LIST: cmd_checkpoint_list(request_id); break;
		case CMD_CHECKPOINT_TOGGLE: cmd_checkpoint_toggle(req, request_id); break;
		case CMD_CHECKPOINT_CONDITION_SET: cmd_checkpoint_condition_set(req, request_id); break;
		case CMD_REGISTERS_GET: cmd_registers_get(req, request_id); break;
		case CMD_REGISTERS_SET: cmd_registers_set(req, request_id); break;
		case CMD_ADVANCE_INSTRUCTIONS: cmd_advance_instructions(req, request_id); break;
		case CMD_EXECUTE_UNTIL_RETURN: cmd_execute_until_return(request_id); break;
		case CMD_PING: send_empty_response(CMD_PING, ERR_OK, request_id); break;
		case CMD_BANKS_AVAILABLE: cmd_banks_available(request_id); break;
		case CMD_REGISTERS_AVAILABLE: cmd_registers_available(req, request_id); break;
		case CMD_VICE_INFO: cmd_vice_info(request_id); break;
		case CMD_EXIT: cmd_exit(request_id); break;
		case CMD_QUIT: cmd_quit(request_id); break;
		case CMD_RESET: cmd_reset(request_id); break;
		case CMD_AUTOSTART: send_empty_response(CMD_AUTOSTART, ERR_OK, request_id); break;
		default:
			send_empty_response(0x00, ERR_INVALID_COMMAND, request_id);
			break;
	}
}

// ------------------------------------------------------------------
// Events
// ------------------------------------------------------------------

void send_pc_event(uint8_t event_type)
{
	body_writer body;
	body.u16(current_pc());
	send_response(event_type, ERR_OK, EVENT_REQUEST_ID, body);
}

// VICE emits an unsolicited REGISTER_INFO whenever the machine stops, and
// clients rely on it to refresh their cached CPU state. VS64's source-line
// step logic reads that cached PC (not the STOPPED event's pc) to decide
// when a step has reached a new line -- without this event the cache stays
// stale, getAddressInfo() never resolves, and stepping single-steps
// forever. So mirror VICE: send REGISTER_INFO before STOPPED.
void send_registers_event()
{
	body_writer body;
	write_registers_body(body);
	send_response(CMD_REGISTERS_GET, ERR_OK, EVENT_REQUEST_ID, body);
}

void stop_event(uint16_t pc)
{
	if (checkpoint_t *cp = find_hit_checkpoint(pc)) {
		++cp->hit_count;
		body_writer info;
		write_checkpoint_info_body(info, *cp, true);
		send_response(RESPONSE_CHECKPOINT_INFO, ERR_OK, EVENT_REQUEST_ID, info);
	}
	send_registers_event();
	send_pc_event(RESPONSE_STOPPED);
}

void process_pause_transitions()
{
	const bool paused = debugger_is_paused();

	// Drive an in-progress line step: keep single-stepping until the source
	// line changes, a breakpoint is hit, or the safety bound is reached.
	if (Line_stepping) {
		if (!paused) {
			return; // the current sub-step is still executing
		}
		const uint16_t pc           = current_pc();
		const int32_t  key          = line_key(pc);
		const bool     line_changed = (key != NO_LINE && key != Line_step_start_key);
		const bool     at_break     = find_hit_checkpoint(pc) != nullptr;
		if (line_changed || at_break || ++Line_step_guard >= LINE_STEP_MAX) {
			Line_stepping = false;
			Was_paused    = true;
			if (Client_socket != INVALID_SOCK) {
				stop_event(pc);
			}
			return;
		}
		// Same source line (or an unmapped region like ROM/library asm):
		// step again without telling the client.
		if (Line_step_over) {
			debugger_step_over_execution();
		} else {
			debugger_step_execution(0);
		}
		Was_paused = false;
		return;
	}

	if (paused == Was_paused) {
		return;
	}
	Was_paused = paused;
	if (Client_socket == INVALID_SOCK) {
		return;
	}
	if (paused) {
		stop_event(current_pc());
	} else {
		send_pc_event(RESPONSE_RESUMED);
	}
}

// ------------------------------------------------------------------
// Receive loop
// ------------------------------------------------------------------

void on_client_disconnected()
{
	close_socket(Client_socket);
	Recv_buffer.clear();
	Line_stepping = false;
	// A client that vanishes must not leave the emulator wedged on orphaned
	// breakpoints with nothing attached to service them.
	const bool had_checkpoints = !Checkpoints.empty();
	remove_all_checkpoints();
	if (had_checkpoints && debugger_is_paused()) {
		debugger_continue_execution();
	}
}

void pump_client()
{
	uint8_t chunk[4096];
	for (;;) {
		const int n = recv(Client_socket, reinterpret_cast<char *>(chunk), sizeof(chunk), 0);
		if (n > 0) {
			Recv_buffer.insert(Recv_buffer.end(), chunk, chunk + n);
			if (n < static_cast<int>(sizeof(chunk))) {
				break;
			}
		} else if (n == 0) {
			on_client_disconnected();
			return;
		} else {
			if (!would_block()) {
				on_client_disconnected();
				return;
			}
			break;
		}
	}

	size_t offset = 0;
	while (Recv_buffer.size() - offset >= 11) {
		const uint8_t *frame = Recv_buffer.data() + offset;
		if (frame[0] != PROTOCOL_STX) {
			// Desynchronized stream; drop the connection rather than guess.
			on_client_disconnected();
			return;
		}
		const uint32_t body_len = static_cast<uint32_t>(frame[2]) | (static_cast<uint32_t>(frame[3]) << 8) |
		                          (static_cast<uint32_t>(frame[4]) << 16) | (static_cast<uint32_t>(frame[5]) << 24);
		if (body_len > 0x100000) {
			on_client_disconnected();
			return;
		}
		if (Recv_buffer.size() - offset < 11 + body_len) {
			break; // incomplete frame; wait for more bytes
		}
		const uint32_t request_id = static_cast<uint32_t>(frame[6]) | (static_cast<uint32_t>(frame[7]) << 8) |
		                            (static_cast<uint32_t>(frame[8]) << 16) | (static_cast<uint32_t>(frame[9]) << 24);
		const uint8_t command = frame[10];
		log_frame("rx", frame, 11 + body_len, command_name(command));
		dispatch(command, request_id, frame + 11, body_len);
		if (Log_file) {
			log_line(fmt::format("     -> pc=${:04x} paused={}", current_pc(), debugger_is_paused() ? 1 : 0));
		}
		offset += 11 + body_len;
		if (Client_socket == INVALID_SOCK) {
			return; // dispatch closed the connection (e.g. send failure)
		}
	}
	if (offset > 0) {
		Recv_buffer.erase(Recv_buffer.begin(), Recv_buffer.begin() + offset);
	}
}

} // namespace

// ------------------------------------------------------------------
// Public API
// ------------------------------------------------------------------

bool binary_monitor_init(const std::string &address)
{
	std::string host = "127.0.0.1";
	uint16_t    port = 6502;

	std::string spec = address;
	if (spec.rfind("ip4://", 0) == 0) {
		spec = spec.substr(6);
	}
	if (!spec.empty()) {
		const size_t colon = spec.rfind(':');
		if (colon != std::string::npos) {
			host = spec.substr(0, colon);
			const int p = atoi(spec.c_str() + colon + 1);
			if (p <= 0 || p > 65535) {
				fmt::print("binary monitor: invalid port in address '{}'\n", address);
				return false;
			}
			port = static_cast<uint16_t>(p);
		} else {
			host = spec;
		}
	}

#ifdef _WIN32
	WSADATA wsa_data;
	if (WSAStartup(MAKEWORD(2, 2), &wsa_data) != 0) {
		fmt::print("binary monitor: WSAStartup failed\n");
		return false;
	}
	// 1 ms scheduler quantum: the paused-loop SDL_Delay(1) between monitor
	// polls must really be ~1 ms (default Windows quantum is 15.6 ms, which
	// made every debug round trip cost a full quantum).
	timeBeginPeriod(1);
#endif

	Listen_socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (Listen_socket == INVALID_SOCK) {
		fmt::print("binary monitor: could not create socket\n");
		return false;
	}

	const int reuse = 1;
	setsockopt(Listen_socket, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char *>(&reuse), sizeof(reuse));

	sockaddr_in addr;
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port   = htons(port);
#ifdef _WIN32
#	pragma warning(push)
#	pragma warning(disable : 4996) // inet_addr: the winsock2 replacement is unreachable here (see include note above)
#endif
	addr.sin_addr.s_addr = inet_addr(host.c_str());
#ifdef _WIN32
#	pragma warning(pop)
#endif
	if (addr.sin_addr.s_addr == INADDR_NONE) {
		fmt::print("binary monitor: invalid host in address '{}'\n", address);
		close_socket(Listen_socket);
		return false;
	}

	if (bind(Listen_socket, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0) {
		fmt::print("binary monitor: could not bind {}:{}\n", host, port);
		close_socket(Listen_socket);
		return false;
	}
	if (listen(Listen_socket, 1) != 0) {
		fmt::print("binary monitor: listen failed on {}:{}\n", host, port);
		close_socket(Listen_socket);
		return false;
	}
	set_nonblocking(Listen_socket);

	Initialized = true;
	Was_paused  = debugger_is_paused();
	log_open();

	// Load Oscar64's source line table (bounce.prg -> bounce.dbj) for
	// line-granular stepping. Absent file just disables that feature.
	if (!Options.prg_path.empty()) {
		std::filesystem::path dbj = Options.prg_path;
		dbj.replace_extension(".dbj");
		load_line_map(dbj.generic_string());
	}

	fmt::print("binary monitor: listening on {}:{}\n", host, port);
	// Tooling (e.g. a VSCode preLaunchTask) watches stdout for the line
	// above to know the monitor is ready; with stdout on a pipe stdio is
	// fully buffered and would sit on it indefinitely.
	fflush(stdout);
	return true;
}

void binary_monitor_shutdown()
{
	if (!Initialized) {
		return;
	}
	close_socket(Client_socket);
	close_socket(Listen_socket);
	Checkpoints.clear();
	Recv_buffer.clear();
	Initialized = false;
#ifdef _WIN32
	timeEndPeriod(1);
	WSACleanup();
#endif
}

void binary_monitor_wait_readable(uint32_t timeout_ms)
{
	// Used by the paused main loop instead of a plain sleep: wakes the
	// moment the client sends a command (a sleep costs a full scheduler
	// quantum, ~15.6 ms on Windows -- per round trip -- since Windows 11
	// ignores timeBeginPeriod for background windows).
	const socket_t s = (Client_socket != INVALID_SOCK) ? Client_socket : Listen_socket;
	if (!Initialized || s == INVALID_SOCK) {
		SDL_Delay(timeout_ms);
		return;
	}
	fd_set readfds;
	FD_ZERO(&readfds);
	FD_SET(s, &readfds);
	timeval tv;
	tv.tv_sec  = 0;
	tv.tv_usec = static_cast<long>(timeout_ms) * 1000;
	select(static_cast<int>(s) + 1, &readfds, nullptr, nullptr, &tv);
}

void binary_monitor_process(bool emulation_paused)
{
	if (!Initialized) {
		return;
	}

	// While the machine runs this is called from the render path; a light
	// time throttle keeps socket syscalls off the hot loop. While paused,
	// poll every call so the debugger feels immediate.
	if (!emulation_paused) {
		static uint32_t last_ticks = 0;
		const uint32_t  now        = SDL_GetTicks();
		if (now - last_ticks < 4) {
			return;
		}
		last_ticks = now;
	}

	if (Client_socket == INVALID_SOCK) {
		Client_socket = accept(Listen_socket, nullptr, nullptr);
		if (Client_socket != INVALID_SOCK) {
			set_nonblocking(Client_socket);
			// Debug traffic is a request/response ping-pong of tiny frames;
			// Nagle + delayed ACK adds ~15-100ms per exchange on loopback.
			const int nodelay = 1;
			setsockopt(Client_socket, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char *>(&nodelay), sizeof(nodelay));
			Recv_buffer.clear();
			Was_paused = debugger_is_paused();
			fmt::print("binary monitor: client connected\n");
		}
	}

	if (Client_socket != INVALID_SOCK) {
		pump_client();
	}

	process_pause_transitions();
}

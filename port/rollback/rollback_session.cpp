/**
 * rollback_session.cpp — rollback netplay for VS battles, driven by GekkoNet.
 *
 * When a VS battle starts in a netplay session, every frame:
 *   1. the local controller is sampled and handed to GekkoNet;
 *   2. GekkoNet answers with Save / Load / Advance events. Saves and loads
 *      go through PortRollbackSave/Load; an Advance stages every player's
 *      input for that tick and runs it — headless while re-simulating,
 *      rendered for the newest tick.
 * Outside a session the frame loop runs one normal tick as before.
 *
 * Transport: UDP natively (local testing); in the browser the page supplies
 * one (WebRTC data channels on the site). Configuration:
 *   SSB64_ROLLBACK=1               enable
 *   SSB64_ROLLBACK_PLAYERS=2       players in the match (2-4)
 *   SSB64_ROLLBACK_LOCAL=0         this instance's player slot
 *   SSB64_ROLLBACK_PORT=7000       local UDP port (native)
 *   SSB64_ROLLBACK_PEERS=a,b,...   address of every slot, in slot order (the
 *                                  local entry is ignored): host:port
 *                                  natively, peer ids in the browser
 *   SSB64_ROLLBACK_PEER=host:port  shorthand for the other slot of a 2-player
 *                                  session
 *   SSB64_ROLLBACK_DELAY=2         local input delay in frames
 *   SSB64_ROLLBACK_WINDOW=8        max prediction (rollback) frames
 *   SSB64_ROLLBACK_SCRIPTED=1      drive the local player from the loaded
 *                                  replay file instead of the controller
 */

#include "rollback_session.h"
#include "rollback_state.h"
#include "port_log.h"

#include <gekkonet.h>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#if defined(__EMSCRIPTEN__)
#include <emscripten.h>
#elif !defined(_WIN32)
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

extern "C" {
void PortRunHeadlessTick(void);
void PortRunRenderedTick(void);

unsigned int syNetInputGetTick(void);
void syNetInputSetSlotSource(int player, int source);
void syNetInputSetSavedInput(int player, unsigned int tick, unsigned short buttons, signed char stick_x,
                             signed char stick_y);

/* Mirrors SYNetInputFrame (decomp/src/sys/netinput.h). */
struct PortNetInputFrame {
	uint32_t tick;
	uint16_t buttons;
	int8_t stick_x;
	int8_t stick_y;
	uint8_t source;
	uint8_t is_predicted;
	uint8_t is_valid;
};
int syNetReplayGetLoadedFrame(int player, unsigned int tick, PortNetInputFrame *out);

/* Mirrors SYNetSyncTickHash (decomp/src/sys/netsync.h). */
struct PortSessionTickHash {
	uint32_t column[12];
};
void syNetSyncHashTick(PortSessionTickHash *out);

/* The decomp's 6-byte OSContPad (PR/os.h). */
struct PortContPad {
	uint16_t button;
	int8_t stick_x;
	int8_t stick_y;
	uint8_t err;
	uint8_t pad;
};
void osContGetReadData(PortContPad *pads);

extern unsigned char gSCManagerSceneData; /* first byte is scene_curr */
extern void *gPortArenaSimStart;
}

namespace {

constexpr unsigned char kSceneVSBattle = 22;
constexpr int kSourceSaved = 3; /* nSYNetInputSourceSaved */
constexpr unsigned kStateCapacity = 4 * 1024 * 1024;

/* What travels over the network per player per frame. */
struct NetInput {
	uint16_t buttons;
	int8_t stick_x;
	int8_t stick_y;
};
static_assert(sizeof(NetInput) == 4, "NetInput must be 4 bytes");

enum class State { Idle, Pending, Running };

struct Config {
	bool enabled = false;
	int players = 2;
	int local = 0;
	int port = 7000;
	std::vector<std::string> peers; /* per slot */
	int delay = 2;
	int window = 8;
	bool scripted = false;
};

Config sConfig;
State sState = State::Idle;
GekkoSession *sSession = nullptr;
bool sStarted = false;
RollbackBuffer sSaveScratch;
unsigned sFrames = 0, sRollbacks = 0, sResimTicks = 0, sStalls = 0, sDesyncs = 0;

/* Bad-network simulation settings (see the UDP transport below). */
int sSimLatency = 0, sSimJitter = 0, sSimLoss = 0;
uint32_t sSimRng = 0x12345678u;

int EnvInt(const char *name, int fallback)
{
	const char *v = std::getenv(name);
	return (v != nullptr && v[0] != '\0') ? std::atoi(v) : fallback;
}

void LoadConfig()
{
	static bool sLoaded = false;
	if (sLoaded) return;
	sLoaded = true;
	sConfig.enabled = EnvInt("SSB64_ROLLBACK", 0) != 0;
	sConfig.players = EnvInt("SSB64_ROLLBACK_PLAYERS", 2);
	sConfig.local = EnvInt("SSB64_ROLLBACK_LOCAL", 0);
	sConfig.port = EnvInt("SSB64_ROLLBACK_PORT", 7000);
	sConfig.peers.assign((size_t)sConfig.players, std::string());
	if (const char *list = std::getenv("SSB64_ROLLBACK_PEERS")) {
		std::string text = list;
		size_t at = 0;
		for (int slot = 0; slot < sConfig.players && at <= text.size(); slot++) {
			size_t comma = text.find(',', at);
			if (comma == std::string::npos) comma = text.size();
			sConfig.peers[(size_t)slot] = text.substr(at, comma - at);
			at = comma + 1;
		}
	} else if (const char *peer = std::getenv("SSB64_ROLLBACK_PEER")) {
		if (sConfig.players == 2 && (sConfig.local == 0 || sConfig.local == 1)) {
			sConfig.peers[(size_t)(1 - sConfig.local)] = peer;
		}
	}
	sConfig.delay = EnvInt("SSB64_ROLLBACK_DELAY", 2);
	sConfig.window = EnvInt("SSB64_ROLLBACK_WINDOW", 8);
	sConfig.scripted = EnvInt("SSB64_ROLLBACK_SCRIPTED", 0) != 0;
#if !defined(_WIN32)
	sSimLatency = EnvInt("SSB64_ROLLBACK_SIM_LATENCY", 0);
	sSimJitter = EnvInt("SSB64_ROLLBACK_SIM_JITTER", 0);
	sSimLoss = EnvInt("SSB64_ROLLBACK_SIM_LOSS", 0);
	sSimRng ^= (uint32_t)sConfig.port * 2654435761u;
#endif
}

/* ------------------------------------------------------------------------- */
/*  Transports                                                               */
/* ------------------------------------------------------------------------- */

/* GekkoNet matches senders by comparing address bytes, so each transport
 * reports a received packet's sender in exactly the form PeerAddress() gives
 * for that slot. Results are malloc'd piecewise; GekkoNet frees each part. */
std::vector<GekkoNetResult *> sReceived;

GekkoNetResult *MakeResult(const void *addr, unsigned addr_len, const void *data, unsigned len)
{
	auto *res = (GekkoNetResult *)std::malloc(sizeof(GekkoNetResult));
	res->addr.data = std::malloc(addr_len);
	std::memcpy(res->addr.data, addr, addr_len);
	res->addr.size = addr_len;
	res->data = std::malloc(len);
	std::memcpy(res->data, data, len);
	res->data_len = len;
	return res;
}

void FreeResult(void *ptr)
{
	std::free(ptr);
}

#if defined(__EMSCRIPTEN__)

/* Browser: the page provides Module.ssbNet = { send(peerId, Uint8Array),
 * inbox: [{ from: peerId, data: Uint8Array }] } before the battle starts —
 * WebRTC data channels on the site, BroadcastChannel in the local test page.
 * Peer ids are short ASCII strings. */
EM_JS(int, web_net_ready, (), {
	return (Module.ssbNet && typeof Module.ssbNet.send === "function") ? 1 : 0;
});

EM_JS(void, web_net_send, (const char *peer, int peer_len, const char *data, int len), {
	Module.ssbNet.send(String.fromCharCode.apply(null, HEAPU8.subarray(peer, peer + peer_len)),
	                   HEAPU8.slice(data, data + len));
});

/* Pops one received packet: writes the sender id (NUL-terminated) and the
 * payload, returns the payload length, or -1 when the inbox is empty. */
EM_JS(int, web_net_pop, (char *peer, int peer_cap, char *buf, int cap), {
	var inbox = Module.ssbNet && Module.ssbNet.inbox;
	if (!inbox || inbox.length === 0) return -1;
	var m = inbox.shift();
	var from = String(m.from);
	var i = 0;
	for (; i < from.length && i < peer_cap - 1; i++) HEAPU8[peer + i] = from.charCodeAt(i) & 0x7f;
	HEAPU8[peer + i] = 0;
	var d = (m.data instanceof Uint8Array) ? m.data : new Uint8Array(m.data);
	var n = Math.min(d.length, cap);
	HEAPU8.set(d.subarray(0, n), buf);
	return n;
});

void WebSend(GekkoNetAddress *addr, const char *data, int length)
{
	web_net_send((const char *)addr->data, (int)addr->size, data, length);
}

GekkoNetResult **WebReceive(int *length)
{
	sReceived.clear();
	char peer[64];
	static char buf[4096];
	for (;;) {
		int n = web_net_pop(peer, (int)sizeof(peer), buf, (int)sizeof(buf));
		if (n < 0) break;
		sReceived.push_back(MakeResult(peer, (unsigned)std::strlen(peer), buf, (unsigned)n));
	}
	*length = (int)sReceived.size();
	return sReceived.data();
}

GekkoNetAdapter sAdapter = { WebSend, WebReceive, FreeResult };

bool OpenTransport()
{
	if (!web_net_ready()) {
		port_log("SSB64 Rollback: page has no Module.ssbNet transport\n");
		return false;
	}
	return true;
}

void CloseTransport()
{
}

GekkoNetAddress PeerAddress(int slot)
{
	const std::string &id = sConfig.peers[(size_t)slot];
	return GekkoNetAddress{ (void *)id.data(), (unsigned)id.size() };
}

#elif !defined(_WIN32)

int sSocket = -1;
std::vector<sockaddr_in> sPeerAddrs; /* per slot */

bool ParseAddress(const std::string &text, sockaddr_in &out)
{
	size_t colon = text.rfind(':');
	if (colon == std::string::npos) return false;
	std::memset(&out, 0, sizeof(out));
#if defined(__APPLE__)
	out.sin_len = sizeof(out);
#endif
	out.sin_family = AF_INET;
	out.sin_port = htons((uint16_t)std::atoi(text.c_str() + colon + 1));
	return inet_pton(AF_INET, text.substr(0, colon).c_str(), &out.sin_addr) == 1;
}

/* Optional bad-network simulation for testing (outgoing packets only):
 *   SSB64_ROLLBACK_SIM_LATENCY=ms  SSB64_ROLLBACK_SIM_JITTER=ms
 *   SSB64_ROLLBACK_SIM_LOSS=percent */
struct DelayedPacket {
	std::chrono::steady_clock::time_point due;
	sockaddr_in to;
	std::vector<char> data;
};
std::vector<DelayedPacket> sDelayed;

uint32_t SimRand()
{
	sSimRng ^= sSimRng << 13;
	sSimRng ^= sSimRng >> 17;
	sSimRng ^= sSimRng << 5;
	return sSimRng;
}

void FlushDelayed()
{
	auto now = std::chrono::steady_clock::now();
	for (size_t i = 0; i < sDelayed.size();) {
		if (sDelayed[i].due <= now) {
			sendto(sSocket, sDelayed[i].data.data(), sDelayed[i].data.size(), 0, (const sockaddr *)&sDelayed[i].to,
			       sizeof(sockaddr_in));
			sDelayed.erase(sDelayed.begin() + (long)i);
		} else {
			i++;
		}
	}
}

void UdpSend(GekkoNetAddress *addr, const char *data, int length)
{
	if (sSocket < 0) return;
	if (sSimLatency == 0 && sSimJitter == 0 && sSimLoss == 0) {
		sendto(sSocket, data, (size_t)length, 0, (const sockaddr *)addr->data, (socklen_t)addr->size);
		return;
	}
	if (sSimLoss > 0 && (int)(SimRand() % 100) < sSimLoss) return;
	int ms = sSimLatency + (sSimJitter > 0 ? (int)(SimRand() % (unsigned)(2 * sSimJitter + 1)) - sSimJitter : 0);
	DelayedPacket pkt;
	pkt.due = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms < 0 ? 0 : ms);
	std::memcpy(&pkt.to, addr->data, sizeof(sockaddr_in));
	pkt.data.assign(data, data + length);
	sDelayed.push_back(std::move(pkt));
}

/* Received addresses are normalized exactly like the configured ones. */
GekkoNetResult **UdpReceive(int *length)
{
	FlushDelayed();
	sReceived.clear();
	for (;;) {
		char buf[2048];
		sockaddr_in from{};
		socklen_t from_len = sizeof(from);
		ssize_t n = recvfrom(sSocket, buf, sizeof(buf), 0, (sockaddr *)&from, &from_len);
		if (n <= 0) break;
		sockaddr_in norm{};
#if defined(__APPLE__)
		norm.sin_len = sizeof(norm);
#endif
		norm.sin_family = AF_INET;
		norm.sin_port = from.sin_port;
		norm.sin_addr = from.sin_addr;
		sReceived.push_back(MakeResult(&norm, sizeof(norm), buf, (unsigned)n));
	}
	*length = (int)sReceived.size();
	return sReceived.data();
}

GekkoNetAdapter sAdapter = { UdpSend, UdpReceive, FreeResult };

bool OpenTransport()
{
	sPeerAddrs.assign((size_t)sConfig.players, sockaddr_in{});
	for (int slot = 0; slot < sConfig.players; slot++) {
		if (slot != sConfig.local && !ParseAddress(sConfig.peers[(size_t)slot], sPeerAddrs[(size_t)slot])) {
			port_log("SSB64 Rollback: slot %d needs a host:port address\n", slot);
			return false;
		}
	}
	sSocket = socket(AF_INET, SOCK_DGRAM, 0);
	if (sSocket < 0) return false;
	sockaddr_in local{};
#if defined(__APPLE__)
	local.sin_len = sizeof(local);
#endif
	local.sin_family = AF_INET;
	local.sin_port = htons((uint16_t)sConfig.port);
	local.sin_addr.s_addr = htonl(INADDR_ANY);
	if (bind(sSocket, (sockaddr *)&local, sizeof(local)) != 0) {
		port_log("SSB64 Rollback: cannot bind UDP port %d\n", sConfig.port);
		close(sSocket);
		sSocket = -1;
		return false;
	}
	fcntl(sSocket, F_SETFL, fcntl(sSocket, F_GETFL) | O_NONBLOCK);
	return true;
}

void CloseTransport()
{
	if (sSocket >= 0) close(sSocket);
	sSocket = -1;
	sDelayed.clear();
}

GekkoNetAddress PeerAddress(int slot)
{
	return GekkoNetAddress{ &sPeerAddrs[(size_t)slot], sizeof(sockaddr_in) };
}

#endif

/* ------------------------------------------------------------------------- */
/*  Session                                                                  */
/* ------------------------------------------------------------------------- */

bool InVSBattle()
{
	return gSCManagerSceneData == kSceneVSBattle && gPortArenaSimStart != nullptr;
}

uint32_t StateChecksum()
{
	PortSessionTickHash h;
	syNetSyncHashTick(&h);
	return h.column[0]; /* folded hash of every gameplay column */
}

void EndSession(const char *why)
{
	if (sSession != nullptr) {
		port_log("SSB64 Rollback: session ended (%s) frames=%u rollbacks=%u resim_ticks=%u stalls=%u desyncs=%u\n",
		         why, sFrames, sRollbacks, sResimTicks, sStalls, sDesyncs);
		gekko_destroy(&sSession);
	}
#if defined(__EMSCRIPTEN__) || !defined(_WIN32)
	CloseTransport();
#endif
	sSession = nullptr;
	sStarted = false;
	sState = State::Idle;
}

bool StartSession()
{
#if defined(_WIN32) && !defined(__EMSCRIPTEN__)
	port_log("SSB64 Rollback: no transport on Windows yet\n");
	return false;
#else
	if (sConfig.players < 2 || sConfig.players > 4 || sConfig.local < 0 || sConfig.local >= sConfig.players) {
		port_log("SSB64 Rollback: bad player setup players=%d local=%d\n", sConfig.players, sConfig.local);
		return false;
	}
	if (!OpenTransport()) {
		return false;
	}
	if (!gekko_create(&sSession, GekkoGameSession)) {
		CloseTransport();
		return false;
	}
	GekkoConfig config{};
	config.num_players = (unsigned char)sConfig.players;
	config.max_spectators = 0;
	config.max_input_prediction_window = (unsigned char)sConfig.window;
	config.input_size = sizeof(NetInput);
	config.state_size = kStateCapacity;
	config.limited_saving = false;
	config.desync_detection = true;
	gekko_start(sSession, &config);
	gekko_net_adapter_set(sSession, &sAdapter);

	/* Every peer adds the slots in the same order: handle == player slot. */
	std::string peers;
	for (int p = 0; p < sConfig.players; p++) {
		if (p == sConfig.local) {
			int handle = gekko_add_actor(sSession, GekkoLocalPlayer, nullptr);
			gekko_set_local_delay(sSession, handle, (unsigned char)sConfig.delay);
		} else {
			GekkoNetAddress addr = PeerAddress(p);
			gekko_add_actor(sSession, GekkoRemotePlayer, &addr);
		}
		peers += (p == 0 ? "" : ",") + (p == sConfig.local ? std::string("(local)") : sConfig.peers[(size_t)p]);
	}
	for (int p = 0; p < 4; p++) {
		syNetInputSetSlotSource(p, kSourceSaved);
	}
	sFrames = sRollbacks = sResimTicks = sStalls = sDesyncs = 0;
	port_log("SSB64 Rollback: session created players=%d local=%d peers=%s delay=%d window=%d scripted=%d\n",
	         sConfig.players, sConfig.local, peers.c_str(), sConfig.delay, sConfig.window, sConfig.scripted ? 1 : 0);
	return true;
#endif
}

NetInput LocalInput()
{
	NetInput in{};
	if (sConfig.scripted) {
		/* GekkoNet files local input under (current frame + delay). */
		PortNetInputFrame frame{};
		unsigned tick = syNetInputGetTick() + (unsigned)sConfig.delay;
		if (syNetReplayGetLoadedFrame(sConfig.local, tick, &frame)) {
			in.buttons = frame.buttons;
			in.stick_x = frame.stick_x;
			in.stick_y = frame.stick_y;
		}
		return in;
	}
	PortContPad pads[4] = {};
	osContGetReadData(pads);
	in.buttons = pads[0].button;
	in.stick_x = pads[0].stick_x;
	in.stick_y = pads[0].stick_y;
	return in;
}

void HandleSessionEvents()
{
	int count = 0;
	GekkoSessionEvent **events = gekko_session_events(sSession, &count);
	for (int i = 0; i < count; i++) {
		GekkoSessionEvent *e = events[i];
		switch (e->type) {
		case GekkoPlayerSyncing:
			port_log("SSB64 Rollback: syncing with player %d (%d/%d)\n", e->data.syncing.handle,
			         e->data.syncing.current, e->data.syncing.max);
			break;
		case GekkoPlayerConnected:
			port_log("SSB64 Rollback: player %d connected\n", e->data.connected.handle);
			break;
		case GekkoPlayerDisconnected:
			port_log("SSB64 Rollback: player %d disconnected\n", e->data.disconnected.handle);
			break;
		case GekkoSessionStarted:
			sStarted = true;
			port_log("SSB64 Rollback: session started\n");
			break;
		case GekkoDesyncDetected:
			sDesyncs++;
			if (sDesyncs <= 10) {
				port_log("SSB64 Rollback: DESYNC frame=%d local=%08X remote=%08X (player %d)\n",
				         e->data.desynced.frame, e->data.desynced.local_checksum,
				         e->data.desynced.remote_checksum, e->data.desynced.remote_handle);
			}
			break;
		default:
			break;
		}
	}
}

void HandleGameEvents()
{
	int count = 0;
	GekkoGameEvent **events = gekko_update_session(sSession, &count);
	bool counted_rollback = false;
	for (int i = 0; i < count; i++) {
		GekkoGameEvent *e = events[i];
		switch (e->type) {
		case GekkoSaveEvent: {
			if (!PortRollbackSave(sSaveScratch) || sSaveScratch.size > kStateCapacity) {
				port_log("SSB64 Rollback: save failed at frame %d (%zu bytes)\n", e->data.save.frame,
				         sSaveScratch.size);
				*e->data.save.state_len = 0;
				break;
			}
			std::memcpy(e->data.save.state, sSaveScratch.data, sSaveScratch.size);
			*e->data.save.state_len = (unsigned)sSaveScratch.size;
			*e->data.save.checksum = StateChecksum();
			break;
		}
		case GekkoLoadEvent:
			if (!PortRollbackLoad(e->data.load.state, e->data.load.state_len)) {
				port_log("SSB64 Rollback: load failed at frame %d\n", e->data.load.frame);
			}
			if (!counted_rollback) {
				sRollbacks++;
				counted_rollback = true;
			}
			break;
		case GekkoAdvanceEvent: {
			unsigned frame = (unsigned)e->data.adv.frame;
			unsigned tick = syNetInputGetTick();
			if (frame != tick) {
				port_log("SSB64 Rollback: frame %u advanced at game tick %u\n", frame, tick);
			}
			const NetInput *inputs = (const NetInput *)e->data.adv.inputs;
			for (int p = 0; p < sConfig.players; p++) {
				syNetInputSetSavedInput(p, tick, inputs[p].buttons, inputs[p].stick_x, inputs[p].stick_y);
			}
			if (e->data.adv.rolling_back) {
				PortRunHeadlessTick();
				sResimTicks++;
			} else {
				PortRunRenderedTick();
			}
			break;
		}
		default:
			break;
		}
	}
}

} /* namespace */

extern "C" void port_rollback_on_battle_start(void)
{
	LoadConfig();
	if (sConfig.enabled) {
		sState = State::Pending;
	}
}

extern "C" int port_rollback_session_frame(void)
{
	if (sState == State::Idle) {
		return 0;
	}
	if (!InVSBattle()) {
		EndSession("battle over");
		return 0;
	}
	if (sState == State::Pending) {
		if (!StartSession()) {
			EndSession("start failed");
			return 0;
		}
		sState = State::Running;
	}

	gekko_network_poll(sSession);
	HandleSessionEvents();

	/* Pacing: if this side runs ahead of the peer, hold one frame now and
	 * then so the other side's inputs can catch up. */
	if (sStarted && (sFrames % 8) == 0 && gekko_frames_ahead(sSession) >= 1.0f) {
		sFrames++;
		sStalls++;
		return 1;
	}

	NetInput input = LocalInput();
	gekko_add_local_input(sSession, sConfig.local, &input);
	HandleGameEvents();
	sFrames++;

	if (sStarted && (sFrames % 600) == 0) {
		GekkoNetworkStats stats{};
		gekko_network_stats(sSession, sConfig.local == 0 ? 1 : 0, &stats);
		port_log("SSB64 Rollback: frame=%u tick=%u ping=%ums avg=%.1fms rollbacks=%u resim_ticks=%u stalls=%u "
		         "desyncs=%u ahead=%.2f\n",
		         sFrames, syNetInputGetTick(), stats.last_ping, stats.avg_ping, sRollbacks, sResimTicks, sStalls,
		         sDesyncs, gekko_frames_ahead(sSession));
	}
	return 1;
}
